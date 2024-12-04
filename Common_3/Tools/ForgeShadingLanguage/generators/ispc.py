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
from utils import isArray, getArrayLen, getArrayBaseName, getMacroName
from utils import getMacroFirstArg, getHeader, getShader, getMacro, platform_langs, get_whitespace
from utils import get_fn_table, Features
import os, re

MAIN_WRAPPER_FSTRING = """export void {}({}uniform int dispatch_x, uniform int dispatch_y, uniform int dispatch_z) {{
    {}
    uniform int total_invocations = dispatch_x * dispatch_y * dispatch_z;
    foreach (invocation = 0 ... total_invocations) {{
        // Calculate coordinates from invocation index
        int x = invocation % dispatch_x;
        int y = (invocation / dispatch_x) % dispatch_y;
        int z = invocation / (dispatch_x * dispatch_y);
        {}
        {}_impl({});
    }}
}}
"""
def extract_function_name(line):
    pattern = r'\s*(?:\w+\s+)*(\w+)\s*\('
    match = re.search(pattern, line)
    if match:
        return str(match.group(1))
    return None

def convert_short_vector_types(line):
    patterns = [
        (r'\bint([234])\b', r'int<\1>'),
        (r'\bfloat([234])\b', r'float<\1>'),
        (r'\buint([234])\b', r'uint<\1>'),
        (r'\bbool([234])\b', r'bool<\1>')
    ]
    result = line
    for pattern, replacement in patterns:
        result = re.sub(pattern, replacement, result)
    
    return result

def convert_vector_constructors(line):
    patterns = [
        (r'\bint<([234])>\(', r'make_int\1('),
        (r'\bfloat<([234])>\(', r'make_float\1('),
        (r'\buint<([234])>\(', r'make_uint\1('),
        (r'\bbool<([234])>\(', r'make_bool\1(')
    ]
    result = line
    for pattern, replacement in patterns:
        result = re.sub(pattern, replacement, result)
    
    return result

def convert_casts(line):
    patterns = [
        (r'\bint\(', r'(int)('),
        (r'\bfloat\(', r'(float)('),
        (r'\buint\(', r'(uint)(')
    ]
    result = line
    for pattern, replacement in patterns:
        result = re.sub(pattern, replacement, result)
    
    return result

def convert_ispc_symbols(line):
    line = convert_short_vector_types(line)
    line = convert_vector_constructors(line)
    line = convert_casts(line)
    return line

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
    main_impl_args = []
    main_str = ""

    parsing_struct = None
    skip_semantics = False

    for fi, line_index, line in iter_lines(shader.lines):

        shader_src_len = len(shader_src)


        if line.strip().startswith('STRUCT('):
            parsing_struct = getMacro(line)
            line = 'struct ' + parsing_struct +  ' {\n'

        if parsing_struct and line.strip().startswith('DATA('):
            data_decl = getMacro(line)
            if skip_semantics or data_decl[-1] == 'None':
                type_name = data_decl[0]
                line = get_whitespace(line) + type_name + ' ' + data_decl[1] + ';\n'

            if Features.INVARIANT in binary.features and data_decl[2].upper() == 'SV_POSITION':
                    line = get_whitespace(line) + 'precise ' + line.strip() + '\n'

        if parsing_struct and '};' in line:

            shader_src += ['// line {}\n'.format(line_index), line]

            skip_semantics = False
            parsing_struct = None
            continue

        resource_decl = None
        if line.strip().startswith('RES('):
            resource_decl = getMacro(line)


        if resource_decl:
            dtype, name, _, _, _ = resource_decl
            base_type = getMacro(dtype)  # Gets inner type
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
            global_var_decl = ""
            if 'CBUFFER' in dtype:
                global_var_decl = f"{base_type} {name};"
            else:
                global_var_decl = f"{base_type} *{base_name};"
            line = f'{global_var_decl} // {line}'

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

        if '_MAIN(' in line:
            leading_args = ''
            global_assignments = ''
            for res in resources:
                # main_impl_args += [res]
                if 'CBUFFER' in res['type']:
                    leading_args += f"uniform const {res['base_type']}& {res['name']}_arg,"
                else:
                    readonly = "const " if res['is_readonly'] else ""
                    leading_args += f"uniform {readonly}{res['base_type']} {res['base_name']}_arg[],"
                global_assignments += f"{res['name']} = {res['name']}_arg;\n    "
            
            name_mangle = binary.filename.split(".")[0].upper()
            main_name = extract_function_name(line) + f'_{name_mangle}'
            line = re.sub(r'_MAIN\(', f'_MAIN_{name_mangle}_impl(', line, count=1)

            for dtype, var in shader.struct_args:
                line = line.replace(dtype+'('+var+')', dtype + ' ' + var)

            for dtype, dvar in shader.flat_args:
                innertype = getMacro(dtype)
                ldtype = line.find(dtype)
                if "SV_DispatchThreadID" in dtype:
                    count = int(innertype[-1:])
                    variable = f"{innertype} {dvar}"
                    if count > 3:
                        print(f"In main function, SV_DispatchThreadID has count {count} but it must be 2 or 3")
                    arg_needed = f"{variable} = {{ {','.join(['x', 'y', 'z'][:count])} }};"
                    main_impl_args += [{
                        "base_type": innertype,
                        "name": dvar,
                        "arg_needed": arg_needed
                    }]
                    line = line[:ldtype]+variable+line[ldtype+len(dtype + ' ' + dvar):]
                else:
                    line = line[:ldtype]+innertype+line[ldtype+len(dtype):]

            impl_args = ",".join([f"{arg['name']}" for arg in main_impl_args])
            impl_declared_vars = "\n".join([arg['arg_needed'] if 'arg_needed' in arg else '' for arg in main_impl_args])
            main_str = convert_ispc_symbols(MAIN_WRAPPER_FSTRING.format(main_name, leading_args, global_assignments, impl_declared_vars, main_name, impl_args))

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

        elif re.match(r'\s*RETURN', line):
            if shader.returnType:
                line = line.replace('RETURN', 'return ')
            else:
                line = line.replace('RETURN()', 'return')

        if 'SET_OUTPUT_FORMAT(' in line:
            line = '//' + line

        if shader_src_len != len(shader_src):
            shader_src += ['//line {}\n'.format(line_index)]

        if re.match(r'#\s*\d+\s+".*"', line):
            # NOTE(joesweeney): These lines seem to include debug info that are fine in most shaders but they
            # probably don't serve a purpose on CPU. Below is an example so future people know what this regex is
            # `# 418 "<built-in>" 3`
            line = ''

        line = convert_ispc_symbols(line)

        shader_src += [line]
    shader_src += [main_str]


    open(dst, 'w').writelines(shader_src)

    return 0, dependencies
