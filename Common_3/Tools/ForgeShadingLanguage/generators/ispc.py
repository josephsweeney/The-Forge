# Copyright (c) 2017-2024 The Forge Interactive Inc.
# 
# This file is part of The-Forge
# (see https://github.com/ConfettiFX/The-Forge).
# 
# Licensed to the Apache Software Foundation (ASF) under one
# or more contributor license agreements.  See the NOTICE file
# distributed with this work for additional information
# regarding copyright ownership.  The ASF licenses this file
# to you under the Apache License, Version 2.0 (the
# "License"); you may not use this file except in compliance
# with the License.  You may obtain a copy of the License at
# 
#   http://www.apache.org/licenses/LICENSE-2.0
# 
# Unless required by applicable law or agreed to in writing,
# software distributed under the License is distributed on an
# "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
# KIND, either express or implied.  See the License for the
# specific language governing permissions and limitations
# under the License.

""" ISPC shader generation """

from utils import Stages, DescriptorSets, WaveopsFlags, ShaderBinary, Platforms, iter_lines
from utils import isArray, getArrayLen, getArrayBaseName, getMacroName, is_groupshared_decl
from utils import getMacroFirstArg, getHeader, getShader, getMacro, platform_langs, get_whitespace
from utils import get_fn_table, Features
import os, re

SHORT_VECTOR_TYPES = {
    "float2": "float<2>",
    "float3": "float<3>",
    "float4": "float<4>",
    "int2": "int<2>",
    "int3": "int<3>",
    "int4": "int<4>",
    "uint2": "uint<2>",
    "uint3": "uint<3>",
    "uint4": "uint<4>",
    "double2": "double<2>",
    "double3": "double<3>",
    "double4": "double<4>",
    "bool2": "bool<2>",
    "bool3": "bool<3>",
    "bool4": "bool<4>",
}

def ispc(*args):
    return ispc_internal(Platforms.ISPC, *args)

def ispc_internal(platform, debug, binary: ShaderBinary, dst):

    fsl = binary.preprocessed_srcs[platform]

    shader = getShader(platform, binary, fsl, dst)
    binary.waveops_flags = shader.waveops_flags
    # check for function overloading.
    get_fn_table(shader.lines)

    shader_src = getHeader(fsl)

    dependencies = []

    # shader_src += [f'#define {platform.name}\n']
    # shader_src += [f'#define {platform_langs[platform]}\n']
        
    shader_src += ['#define STAGE_' + shader.stage.name + '\n']
    if shader.waveops_flags != WaveopsFlags.WAVE_OPS_NONE:
        shader_src += ['#define ENABLE_WAVEOPS(flags)\n']

    # directly embed d3d header in shader
    header_path = os.path.join(os.path.dirname(os.path.dirname(__file__)), 'includes', 'ispc.h')
    header_lines = open(header_path).readlines()
    shader_src += header_lines + ['\n']

    nonuniformresourceindex = None

    resources = []
    arguments_changed = []

    parsing_struct = None
    skip_semantics = False

    for fi, line_index, line in iter_lines(shader.lines):

        shader_src_len = len(shader_src)

        if is_groupshared_decl(line):
            dtype, dname = getMacro(line)
            basename = getArrayBaseName(dname)
            shader_src += ['#define srt_'+basename+' '+basename+'\n']
            line = 'groupshared '+dtype+' '+dname+';\n'

        if line.strip().startswith('STRUCT('):
            parsing_struct = getMacro(line)
            line = 'struct ' + parsing_struct +  ' {\n'

        if parsing_struct and line.strip().startswith('DATA('):
            data_decl = getMacro(line)
            if skip_semantics or data_decl[-1] == 'None':
                type_name = SHORT_VECTOR_TYPES.get(data_decl[0], data_decl[0]) 
                line = get_whitespace(line) + type_name + ' ' + data_decl[1] + ';\n'

            if Features.INVARIANT in binary.features and data_decl[2].upper() == 'SV_POSITION':
                    line = get_whitespace(line) + 'precise ' + line.strip() + '\n'

        if parsing_struct and '};' in line:

            shader_src += ['#line {}\n'.format(line_index), line]

            skip_semantics = False
            parsing_struct = None
            continue

        resource_decl = None
        if line.strip().startswith('RES('):
            resource_decl = getMacro(line)


        if resource_decl:
            dtype, name, _, _, _ = resource_decl
            base_type = getMacro(dtype)  # Gets inner type
            base_type = SHORT_VECTOR_TYPES.get(base_type, base_type)
            base_name = getArrayBaseName(name)
            is_array = isArray(name)
            is_readonly = not ('RW' in dtype or 'W' == dtype[:1])
            resources += [{
                'type': dtype,
                'base_type': base_type,
                'name': name,
                'base_name': base_name,
                'is_array': is_array,
                'is_readonly': is_readonly,
            }]
            line = '// ' + line

        # if '_MAIN(' in line and shader.returnType:
        #     if shader.returnType not in shader.structs:
        #         if shader.stage == Stages.FRAG:
        #             if not 'SV_DEPTH' in shader.returnType.upper():
        #                 line = line.rstrip() + ': SV_TARGET\n'
        #             else:
        #                 line = line.rstrip() + ': SV_DEPTH\n'
        #         if shader.stage == Stages.VERT:
        #             line = line.rstrip() + ': SV_POSITION\n'
        #             if Features.INVARIANT in binary.features:
        #                 line = 'precise ' + line

        # manually transform Type(var) to Type var (necessary for DX11/fxc)
        if '_MAIN(' in line:
            line = 'export ' + line

            leading_args = ''
            for res in resources:
                if 'CBUFFER' in res['type']:
                    leading_args += f"uniform const {res['base_type']}& {res['name']},"
                else:
                    readonly = "const " if res['is_readonly'] else ""
                    leading_args += f"uniform {readonly}{res['base_type']} {res['base_name']}[],"
            line = re.sub(r'_MAIN\(', f'_MAIN({leading_args}', line, count=1)

            for dtype, var in shader.struct_args:
                line = line.replace(dtype+'('+var+')', dtype + ' ' + var)

            for dtype, dvar in shader.flat_args:
                innertype = getMacro(dtype)
                ldtype = line.find(dtype)
                if innertype in SHORT_VECTOR_TYPES:
                    # You can't have vector type args in ISPC
                    numerical_type = innertype[:-1]
                    count = int(innertype[-1:])
                    variable_names = [f"{dvar}_{x}" for x in ['x', 'y', 'z', 'w'][:count]]
                    variables = [f"{numerical_type} {name}" for name in variable_names]
                    arguments_changed += [{
                        "original_type": innertype,
                        "original_name": dvar,
                        "new_names": variable_names,
                    }]
                    line = line[:ldtype]+','.join(variables)+line[ldtype+len(dtype + ' ' + dvar):]
                else:
                    line = line[:ldtype]+innertype+line[ldtype+len(dtype):]

        if 'INIT_MAIN' in line:
            for argument in arguments_changed:
                line += f'{argument["original_type"]} {argument["original_name"]} = ' + '{ ' + ','.join(argument["new_names"]) + ' };'
            # ws = get_whitespace(line)
            # if shader.returnType:
            #     line = ws+'//'+line.strip()+'\n'

        # if 'BeginNonUniformResourceIndex(' in line:
        #     index, max_index = getMacro(line), None
        #     assert index != [], 'No index provided for {}'.format(line)
        #     if type(index) == list:
        #         max_index = index[1]
        #         index = index[0]
        #     nonuniformresourceindex = index
        #     line = '#define {0} NonUniformResourceIndex({0})\n'.format(nonuniformresourceindex)
        # if 'EndNonUniformResourceIndex()' in line:
        #     assert nonuniformresourceindex, 'EndNonUniformResourceIndex: BeginNonUniformResourceIndex not called/found'
        #     line = '#undef {}\n'.format(nonuniformresourceindex)
        #     nonuniformresourceindex = None

        elif re.match('\s*RETURN', line):
            if shader.returnType:
                line = line.replace('RETURN', 'return ')
            else:
                line = line.replace('RETURN()', 'return')

        if 'SET_OUTPUT_FORMAT(' in line:
            line = '//' + line

        if shader_src_len != len(shader_src):
            shader_src += ['#line {}\n'.format(line_index)]

        if re.match(r'#\s*\d+\s+".*"', line):
            # NOTE(joesweeney): These lines seem to include debug info that are fine in most shaders but they
            # probably don't serve a purpose on CPU. Below is an example so future people know what this regex is
            # `# 418 "<built-in>" 3`
            line = ''

        shader_src += [line]

    open(dst, 'w').writelines(shader_src)

    return 0, dependencies
