import json
import logging
from unittest import case
from tasks.ceph_test_case import CephTestCase
import os
import re
from StringIO import StringIO

from tasks.cephfs.fuse_mount import FuseMount

from teuthology.orchestra import run
from teuthology.orchestra.run import CommandFailedError


log = logging.getLogger(__name__)


def for_teuthology(f):
    """
    Decorator that adds an "is_for_teuthology" attribute to the wrapped function
    """
    f.is_for_teuthology = True
    return f


def needs_trimming(f):
    """
    Mark fn as requiring a client capable of trimming its cache (i.e. for ceph-fuse
    this means it needs to be able to run as root, currently)
    """
    f.needs_trimming = True
    return f


class CephFSTestCase(CephTestCase):
    """
    Test case for Ceph FS, requires caller to populate Filesystem and Mounts,
    into the fs, mount_a, mount_b class attributes (setting mount_b is optional)

    Handles resetting the cluster under test between tests.
    """

    # FIXME weird explicit naming
    mount_a = None
    mount_b = None
    recovery_mount = None

    # Declarative test requirements: subclasses should override these to indicate
    # their special needs.  If not met, tests will be skipped.
    CLIENTS_REQUIRED = 1
    MDSS_REQUIRED = 1
    REQUIRE_KCLIENT_REMOTE = False
    REQUIRE_ONE_CLIENT_REMOTE = False
    REQUIRE_MEMSTORE = False

    # Whether to create the default filesystem during setUp
    REQUIRE_FILESYSTEM = True

    # requires REQUIRE_FILESYSTEM = True
    REQUIRE_RECOVERY_FILESYSTEM = False

    LOAD_SETTINGS = []

    def setUp(self):
        super(CephFSTestCase, self).setUp()

        if len(self.mds_cluster.mds_ids) < self.MDSS_REQUIRED:
            raise case.SkipTest("Only have {0} MDSs, require {1}".format(
                len(self.mds_cluster.mds_ids), self.MDSS_REQUIRED
            ))

        if len(self.mounts) < self.CLIENTS_REQUIRED:
            raise case.SkipTest("Only have {0} clients, require {1}".format(
                len(self.mounts), self.CLIENTS_REQUIRED
            ))

        if self.REQUIRE_KCLIENT_REMOTE:
            if not isinstance(self.mounts[0], FuseMount) or not isinstance(self.mounts[1], FuseMount):
                # kclient kill() power cycles nodes, so requires clients to each be on
                # their own node
                if self.mounts[0].client_remote.hostname == self.mounts[1].client_remote.hostname:
                    raise case.SkipTest("kclient clients must be on separate nodes")

        if self.REQUIRE_ONE_CLIENT_REMOTE:
            if self.mounts[0].client_remote.hostname in self.mds_cluster.get_mds_hostnames():
                raise case.SkipTest("Require first client to be on separate server from MDSs")

        if self.REQUIRE_MEMSTORE:
            objectstore = self.mds_cluster.get_config("osd_objectstore", "osd")
            if objectstore != "memstore":
                # You certainly *could* run this on a real OSD, but you don't want to sit
                # here for hours waiting for the test to fill up a 1TB drive!
                raise case.SkipTest("Require `memstore` OSD backend to simulate full drives")

        # Create friendly mount_a, mount_b attrs
        for i in range(0, self.CLIENTS_REQUIRED):
            setattr(self, "mount_{0}".format(chr(ord('a') + i)), self.mounts[i])

        self.mds_cluster.clear_firewall()

        # Unmount all clients, we are about to blow away the filesystem
        for mount in self.mounts:
            if mount.is_mounted():
                mount.umount_wait(force=True)

        # To avoid any issues with e.g. unlink bugs, we destroy and recreate
        # the filesystem rather than just doing a rm -rf of files
        self.mds_cluster.mds_stop()
        self.mds_cluster.mds_fail()
        self.mds_cluster.delete_all_filesystems()
        self.fs = None # is now invalid!
        self.recovery_fs = None

        # In case anything is in the OSD blacklist list, clear it out.  This is to avoid
        # the OSD map changing in the background (due to blacklist expiry) while tests run.
        try:
            self.mds_cluster.mon_manager.raw_cluster_cmd("osd", "blacklist", "clear")
        except CommandFailedError:
            # Fallback for older Ceph cluster
            blacklist = json.loads(self.mds_cluster.mon_manager.raw_cluster_cmd("osd",
                                  "dump", "--format=json-pretty"))['blacklist']
            log.info("Removing {0} blacklist entries".format(len(blacklist)))
            for addr, blacklisted_at in blacklist.items():
                self.mds_cluster.mon_manager.raw_cluster_cmd("osd", "blacklist", "rm", addr)

        client_mount_ids = [m.client_id for m in self.mounts]
        # In case the test changes the IDs of clients, stash them so that we can
        # reset in tearDown
        self._original_client_ids = client_mount_ids
        log.info(client_mount_ids)

        # In case there were any extra auth identities around from a previous
        # test, delete them
        for entry in self.auth_list():
            ent_type, ent_id = entry['entity'].split(".")
            if ent_type == "client" and ent_id not in client_mount_ids and ent_id != "admin":
                self.mds_cluster.mon_manager.raw_cluster_cmd("auth", "del", entry['entity'])

        if self.REQUIRE_FILESYSTEM:
            self.fs = self.mds_cluster.newfs(create=True)
            self.fs.mds_restart()

            # In case some test messed with auth caps, reset them
            for client_id in client_mount_ids:
                self.mds_cluster.mon_manager.raw_cluster_cmd_result(
                    'auth', 'caps', "client.{0}".format(client_id),
                    'mds', 'allow',
                    'mon', 'allow r',
                    'osd', 'allow rw pool={0}'.format(self.fs.get_data_pool_name()))

            # wait for mds restart to complete...
            self.fs.wait_for_daemons()

            # Mount the requested number of clients
            for i in range(0, self.CLIENTS_REQUIRED):
                self.mounts[i].mount()
                self.mounts[i].wait_until_mounted()

        if self.REQUIRE_RECOVERY_FILESYSTEM:
            if not self.REQUIRE_FILESYSTEM:
                raise case.SkipTest("Recovery filesystem requires a primary filesystem as well")
            self.fs.mon_manager.raw_cluster_cmd('fs', 'flag', 'set',
                                                'enable_multiple', 'true',
                                                '--yes-i-really-mean-it')
            self.recovery_fs = self.mds_cluster.newfs(name="recovery_fs", create=False)
            self.recovery_fs.set_metadata_overlay(True)
            self.recovery_fs.set_data_pool_name(self.fs.get_data_pool_name())
            self.recovery_fs.create()
            self.recovery_fs.getinfo(refresh=True)
            self.recovery_fs.mds_restart()
            self.recovery_fs.wait_for_daemons()

        # Load an config settings of interest
        for setting in self.LOAD_SETTINGS:
            setattr(self, setting, float(self.fs.mds_asok(
                ['config', 'get', setting], self.mds_cluster.mds_ids[0]
            )[setting]))

        self.configs_set = set()

    def tearDown(self):
        super(CephFSTestCase, self).tearDown()

        self.mds_cluster.clear_firewall()
        for m in self.mounts:
            m.teardown()

        for i, m in enumerate(self.mounts):
            m.client_id = self._original_client_ids[i]

        for subsys, key in self.configs_set:
            self.mds_cluster.clear_ceph_conf(subsys, key)

    def set_conf(self, subsys, key, value):
        self.configs_set.add((subsys, key))
        self.mds_cluster.set_ceph_conf(subsys, key, value)

    def auth_list(self):
        """
        Convenience wrapper on "ceph auth ls"
        """
        return json.loads(self.mds_cluster.mon_manager.raw_cluster_cmd(
            "auth", "ls", "--format=json-pretty"
        ))['auth_dump']

    def assert_session_count(self, expected, ls_data=None, mds_id=None):
        if ls_data is None:
            ls_data = self.fs.mds_asok(['session', 'ls'], mds_id=mds_id)

        alive_count = len([s for s in ls_data if s['state'] != 'killing'])

        self.assertEqual(expected, alive_count, "Expected {0} sessions, found {1}".format(
            expected, alive_count
        ))

    def assert_session_state(self, client_id,  expected_state):
        self.assertEqual(
            self._session_by_id(
                self.fs.mds_asok(['session', 'ls'])).get(client_id, {'state': None})['state'],
            expected_state)

    def get_session_data(self, client_id):
        return self._session_by_id(client_id)

    def _session_list(self):
        ls_data = self.fs.mds_asok(['session', 'ls'])
        ls_data = [s for s in ls_data if s['state'] not in ['stale', 'closed']]
        return ls_data

    def get_session(self, client_id, session_ls=None):
        if session_ls is None:
            session_ls = self.fs.mds_asok(['session', 'ls'])

        return self._session_by_id(session_ls)[client_id]

    def _session_by_id(self, session_ls):
        return dict([(s['id'], s) for s in session_ls])

    def wait_for_daemon_start(self, daemon_ids=None):
        """
        Wait until all the daemons appear in the FSMap, either assigned
        MDS ranks or in the list of standbys
        """
        def get_daemon_names():
            return [info['name'] for info in self.mds_cluster.status().get_all()]

        if daemon_ids is None:
            daemon_ids = self.mds_cluster.mds_ids

        try:
            self.wait_until_true(
                lambda: set(daemon_ids) & set(get_daemon_names()) == set(daemon_ids),
                timeout=30
            )
        except RuntimeError:
            log.warn("Timeout waiting for daemons {0}, while we have {1}".format(
                daemon_ids, get_daemon_names()
            ))
            raise

    def assert_mds_crash(self, daemon_id):
        """
        Assert that the a particular MDS daemon crashes (block until
        it does)
        """
        try:
            self.mds_cluster.mds_daemons[daemon_id].proc.wait()
        except CommandFailedError as e:
            log.info("MDS '{0}' crashed with status {1} as expected".format(daemon_id, e.exitstatus))
            self.mds_cluster.mds_daemons[daemon_id].proc = None

            # Go remove the coredump from the crash, otherwise teuthology.internal.coredump will
            # catch it later and treat it as a failure.
            p = self.mds_cluster.mds_daemons[daemon_id].remote.run(args=[
                "sudo", "sysctl", "-n", "kernel.core_pattern"], stdout=StringIO())
            core_pattern = p.stdout.getvalue().strip()
            if os.path.dirname(core_pattern):  # Non-default core_pattern with a directory in it
                # We have seen a core_pattern that looks like it's from teuthology's coredump
                # task, so proceed to clear out the core file
                log.info("Clearing core from pattern: {0}".format(core_pattern))

                # Determine the PID of the crashed MDS by inspecting the MDSMap, it had
                # to talk to the mons to get assigned a rank to reach the point of crashing
                addr = self.mds_cluster.mon_manager.get_mds_status(daemon_id)['addr']
                pid_str = addr.split("/")[1]
                log.info("Determined crasher PID was {0}".format(pid_str))

                # Substitute PID into core_pattern to get a glob
                core_glob = core_pattern.replace("%p", pid_str)
                core_glob = re.sub("%[a-z]", "*", core_glob)  # Match all for all other % tokens

                # Verify that we see the expected single coredump matching the expected pattern
                ls_proc = self.mds_cluster.mds_daemons[daemon_id].remote.run(args=[
                    "sudo", "ls", run.Raw(core_glob)
                ], stdout=StringIO())
                cores = [f for f in ls_proc.stdout.getvalue().strip().split("\n") if f]
                log.info("Enumerated cores: {0}".format(cores))
                self.assertEqual(len(cores), 1)

                log.info("Found core file {0}, deleting it".format(cores[0]))

                self.mds_cluster.mds_daemons[daemon_id].remote.run(args=[
                    "sudo", "rm", "-f", cores[0]
                ])
            else:
                log.info("No core_pattern directory set, nothing to clear (internal.coredump not enabled?)")

        else:
            raise AssertionError("MDS daemon '{0}' did not crash as expected".format(daemon_id))
