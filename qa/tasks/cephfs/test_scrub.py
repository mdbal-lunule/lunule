"""
Test CephFS scrub (distinct from OSD scrub) functionality
"""
import logging
import os
import traceback
from collections import namedtuple

from teuthology.orchestra.run import CommandFailedError
from tasks.cephfs.cephfs_test_case import CephFSTestCase, for_teuthology

log = logging.getLogger(__name__)

ValidationError = namedtuple("ValidationError", ["exception", "backtrace"])


class Workload(object):
    def __init__(self, filesystem, mount):
        self._mount = mount
        self._filesystem = filesystem
        self._initial_state = None

        # Accumulate backtraces for every failed validation, and return them.  Backtraces
        # are rather verbose, but we only see them when something breaks, and they
        # let us see which check failed without having to decorate each check with
        # a string
        self._errors = []

    def assert_equal(self, a, b):
        try:
            if a != b:
                raise AssertionError("{0} != {1}".format(a, b))
        except AssertionError as e:
            self._errors.append(
                ValidationError(e, traceback.format_exc(3))
            )

    def write(self):
        """
        Write the workload files to the mount
        """
        raise NotImplementedError()

    def validate(self):
        """
        Read from the mount and validate that the workload files are present (i.e. have
        survived or been reconstructed from the test scenario)
        """
        raise NotImplementedError()

    def damage(self):
        """
        Damage the filesystem pools in ways that will be interesting to recover from.  By
        default just wipe everything in the metadata pool
        """
        # Delete every object in the metadata pool
        objects = self._filesystem.rados(["ls"]).split("\n")
        for o in objects:
            self._filesystem.rados(["rm", o])

    def flush(self):
        """
        Called after client unmount, after write: flush whatever you want
        """
        self._filesystem.mds_asok(["flush", "journal"])


class BacktraceWorkload(Workload):
    """
    Single file, single directory, wipe the backtrace and check it.
    """
    def write(self):
        self._mount.run_shell(["mkdir", "subdir"])
        self._mount.write_n_mb("subdir/sixmegs", 6)

    def validate(self):
        st = self._mount.stat("subdir/sixmegs")
        self._filesystem.mds_asok(["flush", "journal"])
        bt = self._filesystem.read_backtrace(st['st_ino'])
        parent = bt['ancestors'][0]['dname']
        self.assert_equal(parent, "sixmegs")
        return self._errors

    def damage(self):
        st = self._mount.stat("subdir/sixmegs")
        self._filesystem.mds_asok(["flush", "journal"])
        self._filesystem._write_data_xattr(st['st_ino'], "parent", "")


class DupInodeWorkload(Workload):
    """
    Duplicate an inode and try scrubbing it twice."
    """

    def write(self):
        self._mount.run_shell(["mkdir", "parent"])
        self._mount.run_shell(["mkdir", "parent/child"])
        self._mount.write_n_mb("parent/parentfile", 6)
        self._mount.write_n_mb("parent/child/childfile", 6)

    def damage(self):
        temp_bin_path = "/tmp/10000000000.00000000_omap.bin"
        self._mount.umount()
        self._filesystem.mds_asok(["flush", "journal"])
        self._filesystem.mds_stop()
        self._filesystem.rados(["getomapval", "10000000000.00000000",
                                "parentfile_head", temp_bin_path])
        self._filesystem.rados(["setomapval", "10000000000.00000000",
                                "shadow_head"], stdin_file=temp_bin_path)
        self._filesystem.set_ceph_conf('mds', 'mds hack allow loading invalid metadata', True)
        self._filesystem.mds_restart()
        self._filesystem.wait_for_daemons()

    def validate(self):
        self._filesystem.mds_asok(["scrub_path", "/", "recursive", "repair"])
        self.assert_equal(self._filesystem.are_daemons_healthy(), True)
        return self._errors


class TestScrub(CephFSTestCase):
    MDSS_REQUIRED = 1

    def _scrub(self, workload, workers=1):
        """
        That when all objects in metadata pool are removed, we can rebuild a metadata pool
        based on the contents of a data pool, and a client can see and read our files.
        """

        # First, inject some files

        workload.write()

        # are off by default, but in QA we need to explicitly disable them)
        self.fs.set_ceph_conf('mds', 'mds verify scatter', False)
        self.fs.set_ceph_conf('mds', 'mds debug scatterstat', False)

        # Apply any data damage the workload wants
        workload.damage()

        self.fs.mds_asok(["scrub_path", "/", "recursive", "repair"])

        # See that the files are present and correct
        errors = workload.validate()
        if errors:
            log.error("Validation errors found: {0}".format(len(errors)))
            for e in errors:
                log.error(e.exception)
                log.error(e.backtrace)
            raise AssertionError("Validation failed, first error: {0}\n{1}".format(
                errors[0].exception, errors[0].backtrace
            ))

    def test_scrub_backtrace(self):
        self._scrub(BacktraceWorkload(self.fs, self.mount_a))

    def test_scrub_dup_inode(self):
        self._scrub(DupInodeWorkload(self.fs, self.mount_a))
