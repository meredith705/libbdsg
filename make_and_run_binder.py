#!/usr/bin/env python3

import glob
import os
import platform
import sys
import shutil
import subprocess
import re
from contextlib import contextmanager
from distutils.sysconfig import get_python_inc


# Overall script settings
bindings_dir = 'cmake_bindings'
this_project_source = f'{os.getcwd()}/src'
this_project_include = f'{os.getcwd()}/include' 
this_project_namespace_to_bind = 'bdsg'
python_module_name = 'bdsg'

def clone_repos():
    ''' download the most recent copy of binder from git '''
    if not glob.glob("binder"):
        print("Binder not found, cloning repo...")
        subprocess.check_call(['git', 'clone', 'https://github.com/RosettaCommons/binder.git', 'binder'])

def build_binder():
    '''
    check for binder executable in the location we expect it
    if it's not there, build binder with the included script
    :return: location of executable
    '''
    if not glob.glob("./build/*/*/bin/*"):
        print("Binder not compiled, using packaged build.py...")
        os.system(f'{get_python_inc().split("/")[-1]} build.py')
    pybind_source = f'binder/build/pybind11/include'
    return "binder/" + glob.glob('./build/*/*/bin/')[0] + "binder"


@contextmanager
def clean_includes():
    '''
    Goes through source code and replaces all quote-format includes with carrot-style includes on entry.

    Reverts changes on exit.
    '''
    changes_made = dict()
    matcher = re.compile('^\s*#include "')
    # find instances of includes we need to change
#    files = list()
#    searchroot = os.path.abspath(f'{this_project_source}/../')
#    for (root,dirs,fils) in os.walk(searchroot):
#        for fl in fils:
#            if(fl.endswith(("hpp","cpp","h","cc","c")) and ("src" in root or "include" in root)):
#                files.append(root+"/"+fl)
#    print(f'found source files {files}')
#    for filename in files:
    for filename in (glob.glob(f'{this_project_source}/**/*.hpp', recursive=True) + 
                     glob.glob(f'{this_project_source}/**/*.cpp', recursive=True) +
                     glob.glob(f'{this_project_source}/**/*.h', recursive=True) +
                     glob.glob(f'{this_project_source}/**/*.cc', recursive=True) + 
                     glob.glob(f'{this_project_source}/**/*.c', recursive=True) + 
                     glob.glob(f'{this_project_source}/../include/**/*.hpp', recursive=True) + # this format needed to reach headers
                     glob.glob(f'{this_project_source}/../include/**/*.cpp', recursive=True) +
                     glob.glob(f'{this_project_source}/../include/**/*.h', recursive=True) +
                     glob.glob(f'{this_project_source}/../include/**/*.cc', recursive=True) +
                     glob.glob(f'{this_project_source}/../include/**/*.c', recursive=True) +
                     glob.glob(f'{this_project_source}/../build/*/src/*/*.hpp', recursive=True) + 
                     glob.glob(f'{this_project_source}/../build/*/src/*/*.cpp', recursive=True) + 
                     glob.glob(f'{this_project_source}/../build/*/src/*/*.h', recursive=True) + 
                     glob.glob(f'{this_project_source}/../build/*/src/*/*.cc', recursive=True) + 
                     glob.glob(f'{this_project_source}/../build/*/src/*/*.c', recursive=True)):
        changes_made[filename] = list()
        with open(filename, 'r') as fh:
            for line in fh:
                if matcher.match(line):
                    spl = line.split('"')
                    replacement = f'{spl[0]}<{spl[1]}>\n'
                    changes_made[filename].append((line, replacement))
        if not changes_made[filename]:
            del changes_made[filename]
    # edit files we need to alter and then resave them
    for filename in changes_made.keys():
        filedata = ""
        listInd = 0
        with open(filename, 'r') as fh:
            for line in fh:
                if listInd < len(changes_made[filename]) and line == changes_made[filename][listInd][0]:
                    filedata += changes_made[filename][listInd][1]
                    listInd += 1
                else:
                    filedata += line
        with open(filename, 'w') as fh:
            fh.write(filedata)
    try:
        yield
    finally:
        for filename in changes_made.keys():
            filedata = ""
            listInd = 0 
            with open(filename, 'r') as fh:
                for line in fh:
                    if listInd < len(changes_made[filename]) and line == changes_made[filename][listInd][1]:
                        filedata += changes_made[filename][listInd][0]
                        listInd += 1
                    else:
                        filedata += line
            with open(filename, 'w') as fh:
                fh.write(filedata)
        

def make_all_includes():
    ''' generates an .hpp file with all includes in this project that need to be bound '''
    all_includes = []
    all_include_filename = 'all_cmake_includes.hpp'
    for filename in (glob.glob(f'{this_project_source}/**/*.hpp', recursive=True) +
                     glob.glob(f'{this_project_source}/**/*.cpp', recursive=True) +
                     glob.glob(f'{this_project_source}/**/*.h', recursive=True) +
                     glob.glob(f'{this_project_source}/**/*.cc', recursive=True) +
                     glob.glob(f'{this_project_source}/**/*.c', recursive=True)):
#                     glob.glob(f'{this_project_source}/../build/*/src/*/*.hpp', recursive=True) +  
#                     glob.glob(f'{this_project_source}/../build/*/src/*/*.cpp', recursive=True) +  
#                     glob.glob(f'{this_project_source}/../build/*/src/*/*.h', recursive=True) +
#                     glob.glob(f'{this_project_source}/../build/*/src/*/*.cc', recursive=True) + 
#                     glob.glob(f'{this_project_source}/../build/*/src/*/*.c', recursive=True)):
        with open(filename, 'r') as fh:
            for line in fh:
                if line.startswith('#include'):
                    all_includes.append(line.strip())
    all_includes = list(set(all_includes))
    # This is to ensure that the list is always the same and doesn't
    # depend on the filesystem state.  Not technically necessary, but
    # will cause inconsistent errors without it.
    all_includes.sort()
    with open(all_include_filename, 'w') as fh:
        for include in all_includes:
            fh.write(f'{include}\n')
    return all_include_filename


def make_bindings_code(all_includes_fn, binder_executable):
    ''' runs the binder executable with required parameters '''
    shutil.rmtree(bindings_dir, ignore_errors=True)
    os.mkdir(bindings_dir)
    # Find all the include directories for dependencies.
    # Some dependency repos have an include and some have an src/include.
    # TODO: This (and a lot of this script) relies on a cmake build having been done out of "build" beforehand!
    # BBHash and sparsepp have weird project structures and needs to be handled specially.
    proj_include = (glob.glob("build/*/src/*/include") +
                    glob.glob("build/*/src/*/src/include") +
                    ["build/sparsepp-prefix/src/sparsepp",
                     "build/bbhash-prefix/src/bbhash"])
    # proj_include = " -I".join(proj_include)
    proj_include = [f'-I{i}' for i in proj_include]
    command = [binder_executable, "--root-module", python_module_name, "--prefix", f'{os.getcwd()}/{bindings_dir}/', '--bind', this_project_namespace_to_bind, "--config", "config.cfg", all_includes_fn, "--", "-std=c++14", f'-I{this_project_include}']
    if platform.system() == 'Darwin':
        # On (newer) Macs, Binder can't find the C++ STL because it is not in
        # /usr/include but under a weird path returned by xcode-select -p and
        # then /usr/include.  See
        # https://github.com/RosettaCommons/binder/issues/26#issuecomment-322538385
        # and
        # https://developer.apple.com/documentation/xcode_release_notes/xcode_10_release_notes#3035624
        stl_path = os.path.join(subprocess.check_output(['xcode-select', '-p']).decode('utf8').strip(), 'usr', 'include', 'c++', 'v1')
        command.append('-isystem' + stl_path)
        # But we also need the MacOS SDK, which provides e.g. the "real" string.h that this STL depends on
        sdk_path=subprocess.check_output(['xcrun', '-sdk', 'macosx', '--show-sdk-path']).decode('utf8').strip()
        command.append('-isysroot' + sdk_path)
        # Also make sure to look for libomp from macports or homebrew, like CMakeLists.txt does
        command.append('-I/opt/local/include/libomp')
        command.append('-I/usr/local/include')
    command = command + proj_include
    command.append("-DNDEBUG")
    command.append("-v")
    #command = (f'{binder_executable} --root-module {python_module_name} '
    #           f'--prefix {os.getcwd()}/{bindings_dir}/ '
    #           f'--bind {this_project_namespace_to_bind} '
    #           + ('--config config.cfg ') +
    #           f' {all_includes_fn} -- -std=c++14 '
    #           f'-I{this_project_include} {proj_include} -DNDEBUG -v').split()
    print('BINDER COMMAND:', ' '.join(command))
    subprocess.check_call(command)

def main():
    clone_repos()
    os.chdir("binder")
    binder_executable = build_binder()
    os.chdir("..")
    with clean_includes():
        all_includes_fn = make_all_includes()
        make_bindings_code(all_includes_fn, binder_executable)

if __name__ == '__main__':
    main()
