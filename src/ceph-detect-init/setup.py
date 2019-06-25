#
# Copyright (C) 2015 SUSE LINUX GmbH
# Copyright (C) 2015 <contact@redhat.com>
#
# Author: Owen Synge <osynge@suse.com>
# Author: Loic Dachary <loic@dachary.org>
#
# This program is free software: you can redistribute it and/or modify
# it under the terms of the GNU Affero General Public License as
# published by the Free Software Foundation, either version 3 of the
# License, or (at your option) any later version.
#
# This program is distributed in the hope that it will be useful,
# but WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
# GNU Affero General Public License for more details.
#
# You should have received a copy of the GNU Affero General Public License
# along with this program.  If not, see `<http://www.gnu.org/licenses/>`.
#
import os
import sys
from setuptools import setup
from setuptools import find_packages

assert sys.version_info >= (2, 7), \
    "Python version lower than 2.7 is not supported"

def read(fname):
    path = os.path.join(os.path.dirname(__file__), fname)
    f = open(path)
    return f.read()

tests_require = read('test-requirements.txt').split()

setup(
    name='ceph-detect-init',
    version='1.0.1',
    packages=find_packages(),

    author='Owen Synge, Loic Dachary',
    author_email='osynge@suse.de, loic@dachary.org',
    description='display the normalized name of the init system',
    long_description=read('README.rst'),
    license='LGPLv2+',
    keywords='ceph',
    url="https://git.ceph.com/?p=ceph.git;a=summary",

    install_requires=['setuptools'],
    tests_require=tests_require,

    classifiers=[
        'Environment :: Console',
        'Intended Audience :: Information Technology',
        'Intended Audience :: System Administrators',
        'Operating System :: POSIX :: Linux',
        'License :: OSI Approved :: GNU Lesser General Public License v2 or later (LGPLv2+)',
        'Programming Language :: Python',
        'Programming Language :: Python :: 2.7',
        'Programming Language :: Python :: 3',
        'Topic :: Utilities',
    ],

    entry_points={

        'console_scripts': [
            'ceph-detect-init = ceph_detect_init.main:run',
            ],

        },
    )
