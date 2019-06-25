from __future__ import print_function

import os
import pkgutil
import shutil
import subprocess
import sys
import tempfile
import textwrap
from distutils.ccompiler import new_compiler
from distutils.errors import CompileError, LinkError
from distutils.sysconfig import customize_compiler

if not pkgutil.find_loader('setuptools'):
    from distutils.core import setup
    from distutils.extension import Extension
else:
    from setuptools import setup
    from setuptools.extension import Extension

# PEP 440 versioning of the RGW package on PyPI
# Bump this version, after every changeset

__version__ = '2.0.0'


def get_python_flags():
    cflags = {'I': [], 'extras': []}
    ldflags = {'l': [], 'L': [], 'extras': []}

    if os.environ.get('VIRTUAL_ENV', None):
        python = "python"
    else:
        python = 'python' + str(sys.version_info.major) + '.' + str(sys.version_info.minor)

    python_config = python + '-config'

    for cflag in subprocess.check_output(
            [python_config, "--cflags"]
    ).strip().decode('utf-8').split():
        if cflag.startswith('-I'):
            cflags['I'].append(cflag.replace('-I', ''))
        else:
            cflags['extras'].append(cflag)

    for ldflag in subprocess.check_output(
            [python_config, "--ldflags"]
    ).strip().decode('utf-8').split():
        if ldflag.startswith('-l'):
            ldflags['l'].append(ldflag.replace('-l', ''))
        if ldflag.startswith('-L'):
            ldflags['L'].append(ldflag.replace('-L', ''))
        else:
            ldflags['extras'].append(ldflag)

    return {
        'cflags': cflags,
        'ldflags': ldflags
    }


def check_sanity():
    """
    Test if development headers and library for rgw is available by compiling a dummy C program.
    """
    CEPH_SRC_DIR = os.path.join(
        os.path.dirname(os.path.abspath(__file__)),
        '..',
        '..'
    )

    tmp_dir = tempfile.mkdtemp(dir=os.environ.get('TMPDIR', os.path.dirname(__file__)))
    tmp_file = os.path.join(tmp_dir, 'rgw_dummy.c')

    with open(tmp_file, 'w') as fp:
        dummy_prog = textwrap.dedent("""
        #include <stddef.h>
        #include "rados/rgw_file.h"

        int main(void) {
            rgwfile_version(NULL, NULL, NULL);
            return 0;
        }
        """)
        fp.write(dummy_prog)

    compiler = new_compiler()
    customize_compiler(compiler)

    if {'MAKEFLAGS', 'MFLAGS', 'MAKELEVEL'}.issubset(set(os.environ.keys())):
        # The setup.py has been invoked by a top-level Ceph make.
        # Set the appropriate CFLAGS and LDFLAGS

        compiler.set_include_dirs([os.path.join(CEPH_SRC_DIR, 'include')])
        compiler.set_library_dirs([os.environ.get('CEPH_LIBDIR')])

    try:
        compiler.define_macro('_FILE_OFFSET_BITS', '64')

        link_objects = compiler.compile(
            sources=[tmp_file],
            output_dir=tmp_dir,
        )

        compiler.link_executable(
            objects=link_objects,
            output_progname=os.path.join(tmp_dir, 'rgw_dummy'),
            libraries=['rgw', 'rados'],
            output_dir=tmp_dir,
        )

    except CompileError:
        print('\nCompile Error: RGW development headers not found', file=sys.stderr)
        return False
    except LinkError:
        print('\nLink Error: RGW library not found', file=sys.stderr)
        return False
    else:
        return True
    finally:
        shutil.rmtree(tmp_dir)


if 'BUILD_DOC' in os.environ.keys():
    pass
elif check_sanity():
    pass
else:
    sys.exit(1)

cmdclass = {}
try:
    from Cython.Build import cythonize
    from Cython.Distutils import build_ext

    cmdclass = {'build_ext': build_ext}
except ImportError:
    print("WARNING: Cython is not installed.")

    if not os.path.isfile('rgw.c'):
        print('ERROR: Cannot find Cythonized file rgw.c', file=sys.stderr)
        sys.exit(1)
    else:
        def cythonize(x, **kwargs):
            return x

        source = "rgw.c"
else:
    source = "rgw.pyx"

# Disable cythonification if we're not really building anything
if (len(sys.argv) >= 2 and
        any(i in sys.argv[1:] for i in ('--help', 'clean', 'egg_info', '--version')
            )):
    def cythonize(x, **kwargs):
        return x

flags = get_python_flags()

setup(
    name='rgw',
    version=__version__,
    description="Python bindings for the RGW library",
    long_description=(
        "This package contains Python bindings for interacting with the "
        "RGW library. RGW is a Object Storage Gateway "
        "that uses a Ceph Storage Cluster to store its data. The "
        "Ceph Object Storage support S3 and Swift APIs, "
        "and file operations."
    ),
    url='https://github.com/ceph/ceph/tree/master/src/pybind/rgw',
    license='LGPLv2+',
    platforms='Linux',
    ext_modules=cythonize(
        [
            Extension(
                "rgw",
                [source],
                include_dirs=flags['cflags']['I'],
                library_dirs=flags['ldflags']['L'],
                libraries=['rados', 'rgw'] + flags['ldflags']['l'],
                extra_compile_args=flags['cflags']['extras'] + flags['ldflags']['extras'],
            )
        ],
        build_dir=os.environ.get("CYTHON_BUILD_DIR", None),
        include_path=[
            os.path.join(os.path.dirname(__file__), "..", "rados")
        ]
    ),
    classifiers=[
        'Intended Audience :: Developers',
        'Intended Audience :: System Administrators',
        'License :: OSI Approved :: GNU Lesser General Public License v2 or later (LGPLv2+)',
        'Operating System :: POSIX :: Linux',
        'Programming Language :: Cython',
        'Programming Language :: Python :: 2.7',
        'Programming Language :: Python :: 3.4',
        'Programming Language :: Python :: 3.5'
    ],
    cmdclass=cmdclass,
)
