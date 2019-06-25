import json
import random
import string
import sys
import time
import logging

try:
    from itertools import izip_longest as zip_longest
except ImportError:
    from itertools import zip_longest
from itertools import combinations
from cStringIO import StringIO

import boto
import boto.s3.connection
from boto.s3.website import WebsiteConfiguration
from boto.s3.cors import CORSConfiguration

from nose.tools import eq_ as eq
from nose.plugins.attrib import attr
from nose.plugins.skip import SkipTest

from .multisite import Zone

from .conn import get_gateway_connection

class Config:
    """ test configuration """
    def __init__(self, **kwargs):
        # by default, wait up to 5 minutes before giving up on a sync checkpoint
        self.checkpoint_retries = kwargs.get('checkpoint_retries', 60)
        self.checkpoint_delay = kwargs.get('checkpoint_delay', 5)
        # allow some time for realm reconfiguration after changing master zone
        self.reconfigure_delay = kwargs.get('reconfigure_delay', 5)

# rgw multisite tests, written against the interfaces provided in rgw_multi.
# these tests must be initialized and run by another module that provides
# implementations of these interfaces by calling init_multi()
realm = None
user = None
config = None
def init_multi(_realm, _user, _config=None):
    global realm
    realm = _realm
    global user
    user = _user
    global config
    config = _config or Config()
    realm_meta_checkpoint(realm)

def get_realm():
    return realm

log = logging.getLogger(__name__)

num_buckets = 0
run_prefix=''.join(random.choice(string.ascii_lowercase) for _ in range(6))

def get_gateway_connection(gateway, credentials):
    """ connect to the given gateway """
    if gateway.connection is None:
        gateway.connection = boto.connect_s3(
                aws_access_key_id = credentials.access_key,
                aws_secret_access_key = credentials.secret,
                host = gateway.host,
                port = gateway.port,
                is_secure = False,
                calling_format = boto.s3.connection.OrdinaryCallingFormat())
    return gateway.connection

def get_zone_connection(zone, credentials):
    """ connect to the zone's first gateway """
    if isinstance(credentials, list):
        credentials = credentials[0]
    return get_gateway_connection(zone.gateways[0], credentials)

def mdlog_list(zone, period = None):
    cmd = ['mdlog', 'list']
    if period:
        cmd += ['--period', period]
    (mdlog_json, _) = zone.cluster.admin(cmd, read_only=True)
    mdlog_json = mdlog_json.decode('utf-8')
    return json.loads(mdlog_json)

def meta_sync_status(zone):
    while True:
        cmd = ['metadata', 'sync', 'status'] + zone.zone_args()
        meta_sync_status_json, retcode = zone.cluster.admin(cmd, check_retcode=False, read_only=True)
        if retcode == 0:
            break
        assert(retcode == 2) # ENOENT
        time.sleep(5)

def mdlog_autotrim(zone):
    zone.cluster.admin(['mdlog', 'autotrim'])

def bilog_list(zone, bucket, args = None):
    cmd = ['bilog', 'list', '--bucket', bucket] + (args or [])
    bilog, _ = zone.cluster.admin(cmd, read_only=True)
    bilog = bilog.decode('utf-8')
    return json.loads(bilog)

def bilog_autotrim(zone, args = None):
    zone.cluster.admin(['bilog', 'autotrim'] + (args or []))

def parse_meta_sync_status(meta_sync_status_json):
    meta_sync_status_json = meta_sync_status_json.decode('utf-8')
    log.debug('current meta sync status=%s', meta_sync_status_json)
    sync_status = json.loads(meta_sync_status_json)

    sync_info = sync_status['sync_status']['info']
    global_sync_status = sync_info['status']
    num_shards = sync_info['num_shards']
    period = sync_info['period']
    realm_epoch = sync_info['realm_epoch']

    sync_markers=sync_status['sync_status']['markers']
    log.debug('sync_markers=%s', sync_markers)
    assert(num_shards == len(sync_markers))

    markers={}
    for i in range(num_shards):
        # get marker, only if it's an incremental marker for the same realm epoch
        if realm_epoch > sync_markers[i]['val']['realm_epoch'] or sync_markers[i]['val']['state'] == 0:
            markers[i] = ''
        else:
            markers[i] = sync_markers[i]['val']['marker']

    return period, realm_epoch, num_shards, markers

def meta_sync_status(zone):
    for _ in range(config.checkpoint_retries):
        cmd = ['metadata', 'sync', 'status'] + zone.zone_args()
        meta_sync_status_json, retcode = zone.cluster.admin(cmd, check_retcode=False, read_only=True)
        if retcode == 0:
            return parse_meta_sync_status(meta_sync_status_json)
        assert(retcode == 2) # ENOENT
        time.sleep(config.checkpoint_delay)

    assert False, 'failed to read metadata sync status for zone=%s' % zone.name

def meta_master_log_status(master_zone):
    cmd = ['mdlog', 'status'] + master_zone.zone_args()
    mdlog_status_json, retcode = master_zone.cluster.admin(cmd, read_only=True)
    mdlog_status = json.loads(mdlog_status_json.decode('utf-8'))

    markers = {i: s['marker'] for i, s in enumerate(mdlog_status)}
    log.debug('master meta markers=%s', markers)
    return markers

def compare_meta_status(zone, log_status, sync_status):
    if len(log_status) != len(sync_status):
        log.error('len(log_status)=%d, len(sync_status)=%d', len(log_status), len(sync_status))
        return False

    msg = ''
    for i, l, s in zip(log_status, log_status.values(), sync_status.values()):
        if l > s:
            if len(msg):
                msg += ', '
            msg += 'shard=' + str(i) + ' master=' + l + ' target=' + s

    if len(msg) > 0:
        log.warning('zone %s behind master: %s', zone.name, msg)
        return False

    return True

def zone_meta_checkpoint(zone, meta_master_zone = None, master_status = None):
    if not meta_master_zone:
        meta_master_zone = zone.realm().meta_master_zone()
    if not master_status:
        master_status = meta_master_log_status(meta_master_zone)

    current_realm_epoch = realm.current_period.data['realm_epoch']

    log.info('starting meta checkpoint for zone=%s', zone.name)

    for _ in range(config.checkpoint_retries):
        period, realm_epoch, num_shards, sync_status = meta_sync_status(zone)
        if realm_epoch < current_realm_epoch:
            log.warning('zone %s is syncing realm epoch=%d, behind current realm epoch=%d',
                        zone.name, realm_epoch, current_realm_epoch)
        else:
            log.debug('log_status=%s', master_status)
            log.debug('sync_status=%s', sync_status)
            if compare_meta_status(zone, master_status, sync_status):
                log.info('finish meta checkpoint for zone=%s', zone.name)
                return

        time.sleep(config.checkpoint_delay)
    assert False, 'failed meta checkpoint for zone=%s' % zone.name

def zonegroup_meta_checkpoint(zonegroup, meta_master_zone = None, master_status = None):
    if not meta_master_zone:
        meta_master_zone = zonegroup.realm().meta_master_zone()
    if not master_status:
        master_status = meta_master_log_status(meta_master_zone)

    for zone in zonegroup.zones:
        if zone == meta_master_zone:
            continue
        zone_meta_checkpoint(zone, meta_master_zone, master_status)

def realm_meta_checkpoint(realm):
    log.info('meta checkpoint')

    meta_master_zone = realm.meta_master_zone()
    master_status = meta_master_log_status(meta_master_zone)

    for zonegroup in realm.current_period.zonegroups:
        zonegroup_meta_checkpoint(zonegroup, meta_master_zone, master_status)

def parse_data_sync_status(data_sync_status_json):
    data_sync_status_json = data_sync_status_json.decode('utf-8')
    log.debug('current data sync status=%s', data_sync_status_json)
    sync_status = json.loads(data_sync_status_json)

    global_sync_status=sync_status['sync_status']['info']['status']
    num_shards=sync_status['sync_status']['info']['num_shards']

    sync_markers=sync_status['sync_status']['markers']
    log.debug('sync_markers=%s', sync_markers)
    assert(num_shards == len(sync_markers))

    markers={}
    for i in range(num_shards):
        markers[i] = sync_markers[i]['val']['marker']

    return (num_shards, markers)

def data_sync_status(target_zone, source_zone):
    if target_zone == source_zone:
        return None

    for _ in range(config.checkpoint_retries):
        cmd = ['data', 'sync', 'status'] + target_zone.zone_args()
        cmd += ['--source-zone', source_zone.name]
        data_sync_status_json, retcode = target_zone.cluster.admin(cmd, check_retcode=False, read_only=True)
        if retcode == 0:
            return parse_data_sync_status(data_sync_status_json)

        assert(retcode == 2) # ENOENT
        time.sleep(config.checkpoint_delay)

    assert False, 'failed to read data sync status for target_zone=%s source_zone=%s' % \
                  (target_zone.name, source_zone.name)

def bucket_sync_status(target_zone, source_zone, bucket_name):
    if target_zone == source_zone:
        return None

    cmd = ['bucket', 'sync', 'status'] + target_zone.zone_args()
    cmd += ['--source-zone', source_zone.name]
    cmd += ['--bucket', bucket_name]
    while True:
        bucket_sync_status_json, retcode = target_zone.cluster.admin(cmd, check_retcode=False, read_only=True)
        if retcode == 0:
            break

        assert(retcode == 2) # ENOENT

    bucket_sync_status_json = bucket_sync_status_json.decode('utf-8')
    log.debug('current bucket sync status=%s', bucket_sync_status_json)
    sync_status = json.loads(bucket_sync_status_json)

    markers={}
    for entry in sync_status:
        val = entry['val']
        if val['status'] == 'incremental-sync':
            pos = val['inc_marker']['position'].split('#')[-1] # get rid of shard id; e.g., 6#00000000002.132.3 -> 00000000002.132.3
        else:
            pos = ''
        markers[entry['key']] = pos

    return markers

def data_source_log_status(source_zone):
    source_cluster = source_zone.cluster
    cmd = ['datalog', 'status'] + source_zone.zone_args()
    datalog_status_json, retcode = source_cluster.rgw_admin(cmd, read_only=True)
    datalog_status = json.loads(datalog_status_json.decode('utf-8'))

    markers = {i: s['marker'] for i, s in enumerate(datalog_status)}
    log.debug('data markers for zone=%s markers=%s', source_zone.name, markers)
    return markers

def bucket_source_log_status(source_zone, bucket_name):
    cmd = ['bilog', 'status'] + source_zone.zone_args()
    cmd += ['--bucket', bucket_name]
    source_cluster = source_zone.cluster
    bilog_status_json, retcode = source_cluster.admin(cmd, read_only=True)
    bilog_status = json.loads(bilog_status_json.decode('utf-8'))

    m={}
    markers={}
    try:
        m = bilog_status['markers']
    except:
        pass

    for s in m:
        key = s['key']
        val = s['val']
        markers[key] = val

    log.debug('bilog markers for zone=%s bucket=%s markers=%s', source_zone.name, bucket_name, markers)
    return markers

def compare_data_status(target_zone, source_zone, log_status, sync_status):
    if len(log_status) != len(sync_status):
        log.error('len(log_status)=%d len(sync_status)=%d', len(log_status), len(sync_status))
        return False

    msg =  ''
    for i, l, s in zip(log_status, log_status.values(), sync_status.values()):
        if l > s:
            if len(msg):
                msg += ', '
            msg += 'shard=' + str(i) + ' master=' + l + ' target=' + s

    if len(msg) > 0:
        log.warning('data of zone %s behind zone %s: %s', target_zone.name, source_zone.name, msg)
        return False

    return True

def compare_bucket_status(target_zone, source_zone, bucket_name, log_status, sync_status):
    if len(log_status) != len(sync_status):
        log.error('len(log_status)=%d len(sync_status)=%d', len(log_status), len(sync_status))
        return False

    msg =  ''
    for i, l, s in zip(log_status, log_status.values(), sync_status.values()):
        if l > s:
            if len(msg):
                msg += ', '
            msg += 'shard=' + str(i) + ' master=' + l + ' target=' + s

    if len(msg) > 0:
        log.warning('bucket %s zone %s behind zone %s: %s', bucket_name, target_zone.name, source_zone.name, msg)
        return False

    return True

def zone_data_checkpoint(target_zone, source_zone_conn):
    if target_zone == source_zone:
        return

    log_status = data_source_log_status(source_zone)
    log.info('starting data checkpoint for target_zone=%s source_zone=%s', target_zone.name, source_zone.name)

    for _ in range(config.checkpoint_retries):
        num_shards, sync_status = data_sync_status(target_zone, source_zone)

        log.debug('log_status=%s', log_status)
        log.debug('sync_status=%s', sync_status)

        if compare_data_status(target_zone, source_zone, log_status, sync_status):
            log.info('finished data checkpoint for target_zone=%s source_zone=%s',
                     target_zone.name, source_zone.name)
            return
        time.sleep(config.checkpoint_delay)

    assert False, 'failed data checkpoint for target_zone=%s source_zone=%s' % \
                  (target_zone.name, source_zone.name)


def zone_bucket_checkpoint(target_zone, source_zone, bucket_name):
    if target_zone == source_zone:
        return

    log_status = bucket_source_log_status(source_zone, bucket_name)
    log.info('starting bucket checkpoint for target_zone=%s source_zone=%s bucket=%s', target_zone.name, source_zone.name, bucket_name)

    for _ in range(config.checkpoint_retries):
        sync_status = bucket_sync_status(target_zone, source_zone, bucket_name)

        log.debug('log_status=%s', log_status)
        log.debug('sync_status=%s', sync_status)

        if compare_bucket_status(target_zone, source_zone, bucket_name, log_status, sync_status):
            log.info('finished bucket checkpoint for target_zone=%s source_zone=%s bucket=%s', target_zone.name, source_zone.name, bucket_name)
            return

        time.sleep(config.checkpoint_delay)

    assert False, 'finished bucket checkpoint for target_zone=%s source_zone=%s bucket=%s' % \
                  (target_zone.name, source_zone.name, bucket_name)

def zonegroup_bucket_checkpoint(zonegroup_conns, bucket_name):
    for source_conn in zonegroup_conns.zones:
        for target_conn in zonegroup_conns.zones:
            if source_conn.zone == target_conn.zone:
                continue
            zone_bucket_checkpoint(target_conn.zone, source_conn.zone, bucket_name)
            target_conn.check_bucket_eq(source_conn, bucket_name)

def set_master_zone(zone):
    zone.modify(zone.cluster, ['--master'])
    zonegroup = zone.zonegroup
    zonegroup.period.update(zone, commit=True)
    zonegroup.master_zone = zone
    log.info('Set master zone=%s, waiting %ds for reconfiguration..', zone.name, config.reconfigure_delay)
    time.sleep(config.reconfigure_delay)

def enable_bucket_sync(zone, bucket_name):
    cmd = ['bucket', 'sync', 'enable', '--bucket', bucket_name] + zone.zone_args()
    zone.cluster.admin(cmd)

def disable_bucket_sync(zone, bucket_name):
    cmd = ['bucket', 'sync', 'disable', '--bucket', bucket_name] + zone.zone_args()
    zone.cluster.admin(cmd)

def check_buckets_sync_status_obj_not_exist(zone, buckets):
    for _ in range(config.checkpoint_retries):
        cmd = ['log', 'list'] + zone.zone_arg()
        log_list, ret = zone.cluster.admin(cmd, check_retcode=False, read_only=True)
        for bucket in buckets:
            if log_list.find(':'+bucket+":") >= 0:
                break
        else:
            return
        time.sleep(config.checkpoint_delay)
    assert False

def gen_bucket_name():
    global num_buckets

    num_buckets += 1
    return run_prefix + '-' + str(num_buckets)

class ZonegroupConns:
    def __init__(self, zonegroup):
        self.zonegroup = zonegroup
        self.zones = []
        self.ro_zones = []
        self.rw_zones = []
        self.master_zone = None
        for z in zonegroup.zones:
            zone_conn = z.get_conn(user.credentials)
            self.zones.append(zone_conn)
            if z.is_read_only():
                self.ro_zones.append(zone_conn)
            else:
                self.rw_zones.append(zone_conn)

            if z == zonegroup.master_zone:
                self.master_zone = zone_conn

def check_all_buckets_exist(zone_conn, buckets):
    if not zone_conn.zone.has_buckets():
        return True

    for b in buckets:
        try:
            zone_conn.get_bucket(b)
        except:
            log.critical('zone %s does not contain bucket %s', zone.name, b)
            return False

    return True

def check_all_buckets_dont_exist(zone_conn, buckets):
    if not zone_conn.zone.has_buckets():
        return True

    for b in buckets:
        try:
            zone_conn.get_bucket(b)
        except:
            continue

        log.critical('zone %s contains bucket %s', zone.zone, b)
        return False

    return True

def create_bucket_per_zone(zonegroup_conns, buckets_per_zone = 1):
    buckets = []
    zone_bucket = []
    for zone in zonegroup_conns.rw_zones:
        for i in xrange(buckets_per_zone):
            bucket_name = gen_bucket_name()
            log.info('create bucket zone=%s name=%s', zone.name, bucket_name)
            bucket = zone.create_bucket(bucket_name)
            buckets.append(bucket_name)
            zone_bucket.append((zone, bucket))

    return buckets, zone_bucket

def create_bucket_per_zone_in_realm():
    buckets = []
    zone_bucket = []
    for zonegroup in realm.current_period.zonegroups:
        zg_conn = ZonegroupConns(zonegroup)
        b, z = create_bucket_per_zone(zg_conn)
        buckets.extend(b)
        zone_bucket.extend(z)
    return buckets, zone_bucket

def test_bucket_create():
    zonegroup = realm.master_zonegroup()
    zonegroup_conns = ZonegroupConns(zonegroup)
    buckets, _ = create_bucket_per_zone(zonegroup_conns)
    zonegroup_meta_checkpoint(zonegroup)

    for zone in zonegroup_conns.zones:
        assert check_all_buckets_exist(zone, buckets)

def test_bucket_recreate():
    zonegroup = realm.master_zonegroup()
    zonegroup_conns = ZonegroupConns(zonegroup)
    buckets, _ = create_bucket_per_zone(zonegroup_conns)
    zonegroup_meta_checkpoint(zonegroup)


    for zone in zonegroup_conns.zones:
        assert check_all_buckets_exist(zone, buckets)

    # recreate buckets on all zones, make sure they weren't removed
    for zone in zonegroup_conns.rw_zones:
        for bucket_name in buckets:
            bucket = zone.create_bucket(bucket_name)

    for zone in zonegroup_conns.zones:
        assert check_all_buckets_exist(zone, buckets)

    zonegroup_meta_checkpoint(zonegroup)

    for zone in zonegroup_conns.zones:
        assert check_all_buckets_exist(zone, buckets)

def test_bucket_remove():
    zonegroup = realm.master_zonegroup()
    zonegroup_conns = ZonegroupConns(zonegroup)
    buckets, zone_bucket = create_bucket_per_zone(zonegroup_conns)
    zonegroup_meta_checkpoint(zonegroup)

    for zone in zonegroup_conns.zones:
        assert check_all_buckets_exist(zone, buckets)

    for zone, bucket_name in zone_bucket:
        zone.conn.delete_bucket(bucket_name)

    zonegroup_meta_checkpoint(zonegroup)

    for zone in zonegroup_conns.zones:
        assert check_all_buckets_dont_exist(zone, buckets)

def get_bucket(zone, bucket_name):
    return zone.conn.get_bucket(bucket_name)

def get_key(zone, bucket_name, obj_name):
    b = get_bucket(zone, bucket_name)
    return b.get_key(obj_name)

def new_key(zone, bucket_name, obj_name):
    b = get_bucket(zone, bucket_name)
    return b.new_key(obj_name)

def check_bucket_eq(zone_conn1, zone_conn2, bucket):
    return zone_conn2.check_bucket_eq(zone_conn1, bucket.name)

def test_object_sync():
    zonegroup = realm.master_zonegroup()
    zonegroup_conns = ZonegroupConns(zonegroup)
    buckets, zone_bucket = create_bucket_per_zone(zonegroup_conns)

    objnames = [ 'myobj', '_myobj', ':', '&' ]
    content = 'asdasd'

    # don't wait for meta sync just yet
    for zone, bucket_name in zone_bucket:
        for objname in objnames:
            k = new_key(zone, bucket_name, objname)
            k.set_contents_from_string(content)

    zonegroup_meta_checkpoint(zonegroup)

    for source_conn, bucket in zone_bucket:
        for target_conn in zonegroup_conns.zones:
            if source_conn.zone == target_conn.zone:
                continue

            zone_bucket_checkpoint(target_conn.zone, source_conn.zone, bucket.name)
            check_bucket_eq(source_conn, target_conn, bucket)

def test_object_delete():
    zonegroup = realm.master_zonegroup()
    zonegroup_conns = ZonegroupConns(zonegroup)
    buckets, zone_bucket = create_bucket_per_zone(zonegroup_conns)

    objname = 'myobj'
    content = 'asdasd'

    # don't wait for meta sync just yet
    for zone, bucket in zone_bucket:
        k = new_key(zone, bucket, objname)
        k.set_contents_from_string(content)

    zonegroup_meta_checkpoint(zonegroup)

    # check object exists
    for source_conn, bucket in zone_bucket:
        for target_conn in zonegroup_conns.zones:
            if source_conn.zone == target_conn.zone:
                continue

            zone_bucket_checkpoint(target_conn.zone, source_conn.zone, bucket.name)
            check_bucket_eq(source_conn, target_conn, bucket)

    # check object removal
    for source_conn, bucket in zone_bucket:
        k = get_key(source_conn, bucket, objname)
        k.delete()
        for target_conn in zonegroup_conns.zones:
            if source_conn.zone == target_conn.zone:
                continue

            zone_bucket_checkpoint(target_conn.zone, source_conn.zone, bucket.name)
            check_bucket_eq(source_conn, target_conn, bucket)

def get_latest_object_version(key):
    for k in key.bucket.list_versions(key.name):
        if k.is_latest:
            return k
    return None

def test_versioned_object_incremental_sync():
    zonegroup = realm.master_zonegroup()
    zonegroup_conns = ZonegroupConns(zonegroup)
    buckets, zone_bucket = create_bucket_per_zone(zonegroup_conns)

    # enable versioning
    for _, bucket in zone_bucket:
        bucket.configure_versioning(True)

    zonegroup_meta_checkpoint(zonegroup)

    # upload a dummy object to each bucket and wait for sync. this forces each
    # bucket to finish a full sync and switch to incremental
    for source_conn, bucket in zone_bucket:
        new_key(source_conn, bucket, 'dummy').set_contents_from_string('')
        for target_conn in zonegroup_conns.zones:
            if source_conn.zone == target_conn.zone:
                continue
            zone_bucket_checkpoint(target_conn.zone, source_conn.zone, bucket.name)

    for _, bucket in zone_bucket:
        # create and delete multiple versions of an object from each zone
        for zone_conn in zonegroup_conns.rw_zones:
            obj = 'obj-' + zone_conn.name
            k = new_key(zone_conn, bucket, obj)

            k.set_contents_from_string('version1')
            v = get_latest_object_version(k)
            log.debug('version1 id=%s', v.version_id)
            # don't delete version1 - this tests that the initial version
            # doesn't get squashed into later versions

            # create and delete the following object versions to test that
            # the operations don't race with each other during sync
            k.set_contents_from_string('version2')
            v = get_latest_object_version(k)
            log.debug('version2 id=%s', v.version_id)
            k.bucket.delete_key(obj, version_id=v.version_id)

            k.set_contents_from_string('version3')
            v = get_latest_object_version(k)
            log.debug('version3 id=%s', v.version_id)
            k.bucket.delete_key(obj, version_id=v.version_id)

    for source_conn, bucket in zone_bucket:
        for target_conn in zonegroup_conns.zones:
            if source_conn.zone == target_conn.zone:
                continue
            zone_bucket_checkpoint(target_conn.zone, source_conn.zone, bucket.name)
            check_bucket_eq(source_conn, target_conn, bucket)

def test_bucket_versioning():
    buckets, zone_bucket = create_bucket_per_zone_in_realm()
    for _, bucket in zone_bucket:
        bucket.configure_versioning(True)
        res = bucket.get_versioning_status()
        key = 'Versioning'
        assert(key in res and res[key] == 'Enabled')

def test_bucket_acl():
    buckets, zone_bucket = create_bucket_per_zone_in_realm()
    for _, bucket in zone_bucket:
        assert(len(bucket.get_acl().acl.grants) == 1) # single grant on owner
        bucket.set_acl('public-read')
        assert(len(bucket.get_acl().acl.grants) == 2) # new grant on AllUsers

def test_bucket_cors():
    buckets, zone_bucket = create_bucket_per_zone_in_realm()
    for _, bucket in zone_bucket:
        cors_cfg = CORSConfiguration()
        cors_cfg.add_rule(['DELETE'], 'https://www.example.com', allowed_header='*', max_age_seconds=3000)
        bucket.set_cors(cors_cfg)
        assert(bucket.get_cors().to_xml() == cors_cfg.to_xml())

def test_bucket_delete_notempty():
    zonegroup = realm.master_zonegroup()
    zonegroup_conns = ZonegroupConns(zonegroup)
    buckets, zone_bucket = create_bucket_per_zone(zonegroup_conns)
    zonegroup_meta_checkpoint(zonegroup)

    for zone_conn, bucket_name in zone_bucket:
        # upload an object to each bucket on its own zone
        conn = zone_conn.get_connection()
        bucket = conn.get_bucket(bucket_name)
        k = bucket.new_key('foo')
        k.set_contents_from_string('bar')
        # attempt to delete the bucket before this object can sync
        try:
            conn.delete_bucket(bucket_name)
        except boto.exception.S3ResponseError as e:
            assert(e.error_code == 'BucketNotEmpty')
            continue
        assert False # expected 409 BucketNotEmpty

    # assert that each bucket still exists on the master
    c1 = zonegroup_conns.master_zone.conn
    for _, bucket_name in zone_bucket:
        assert c1.get_bucket(bucket_name)

def test_multi_period_incremental_sync():
    zonegroup = realm.master_zonegroup()
    if len(zonegroup.zones) < 3:
        raise SkipTest("test_multi_period_incremental_sync skipped. Requires 3 or more zones in master zonegroup.")

    # periods to include in mdlog comparison
    mdlog_periods = [realm.current_period.id]

    # create a bucket in each zone
    zonegroup_conns = ZonegroupConns(zonegroup)
    buckets, zone_bucket = create_bucket_per_zone(zonegroup_conns)

    zonegroup_meta_checkpoint(zonegroup)

    z1, z2, z3 = zonegroup.zones[0:3]
    assert(z1 == zonegroup.master_zone)

    # kill zone 3 gateways to freeze sync status to incremental in first period
    z3.stop()

    # change master to zone 2 -> period 2
    set_master_zone(z2)
    mdlog_periods += [realm.current_period.id]

    for zone_conn, _ in zone_bucket:
        if zone_conn.zone == z3:
            continue
        bucket_name = gen_bucket_name()
        log.info('create bucket zone=%s name=%s', zone_conn.name, bucket_name)
        bucket = zone_conn.conn.create_bucket(bucket_name)
        buckets.append(bucket_name)

    # wait for zone 1 to sync
    zone_meta_checkpoint(z1)

    # change master back to zone 1 -> period 3
    set_master_zone(z1)
    mdlog_periods += [realm.current_period.id]

    for zone_conn, bucket_name in zone_bucket:
        if zone_conn.zone == z3:
            continue
        bucket_name = gen_bucket_name()
        log.info('create bucket zone=%s name=%s', zone_conn.name, bucket_name)
        bucket = zone_conn.conn.create_bucket(bucket_name)
        buckets.append(bucket_name)

    # restart zone 3 gateway and wait for sync
    z3.start()
    zonegroup_meta_checkpoint(zonegroup)

    # verify that we end up with the same objects
    for bucket_name in buckets:
        for source_conn, _ in zone_bucket:
            for target_conn in zonegroup_conns.zones:
                if source_conn.zone == target_conn.zone:
                    continue

                target_conn.check_bucket_eq(source_conn, bucket_name)

    # verify that mdlogs are not empty and match for each period
    for period in mdlog_periods:
        master_mdlog = mdlog_list(z1, period)
        assert len(master_mdlog) > 0
        for zone in zonegroup.zones:
            if zone == z1:
                continue
            mdlog = mdlog_list(zone, period)
            assert len(mdlog) == len(master_mdlog)

    # autotrim mdlogs for master zone
    mdlog_autotrim(z1)

    # autotrim mdlogs for peers
    for zone in zonegroup.zones:
        if zone == z1:
            continue
        mdlog_autotrim(zone)

    # verify that mdlogs are empty for each period
    for period in mdlog_periods:
        for zone in zonegroup.zones:
            mdlog = mdlog_list(zone, period)
            assert len(mdlog) == 0

def test_zonegroup_remove():
    zonegroup = realm.master_zonegroup()
    zonegroup_conns = ZonegroupConns(zonegroup)
    if len(zonegroup.zones) < 2:
        raise SkipTest("test_zonegroup_remove skipped. Requires 2 or more zones in master zonegroup.")

    zonegroup_meta_checkpoint(zonegroup)
    z1, z2 = zonegroup.zones[0:2]
    c1, c2 = (z1.cluster, z2.cluster)

    # create a new zone in zonegroup on c2 and commit
    zone = Zone('remove', zonegroup, c2)
    zone.create(c2)
    zonegroup.zones.append(zone)
    zonegroup.period.update(zone, commit=True)

    zonegroup.remove(c1, zone)

    # another 'zonegroup remove' should fail with ENOENT
    _, retcode = zonegroup.remove(c1, zone, check_retcode=False)
    assert(retcode == 2) # ENOENT

    # delete the new zone
    zone.delete(c2)

    # validate the resulting period
    zonegroup.period.update(z1, commit=True)

def test_set_bucket_website():
    buckets, zone_bucket = create_bucket_per_zone_in_realm()
    for _, bucket in zone_bucket:
        website_cfg = WebsiteConfiguration(suffix='index.html',error_key='error.html')
        try:
            bucket.set_website_configuration(website_cfg)
        except boto.exception.S3ResponseError as e:
            if e.error_code == 'MethodNotAllowed':
                raise SkipTest("test_set_bucket_website skipped. Requires rgw_enable_static_website = 1.")
        assert(bucket.get_website_configuration_with_xml()[1] == website_cfg.to_xml())

def test_set_bucket_policy():
    policy = '''{
  "Version": "2012-10-17",
  "Statement": [{
    "Effect": "Allow",
    "Principal": "*"
  }]
}'''
    buckets, zone_bucket = create_bucket_per_zone_in_realm()
    for _, bucket in zone_bucket:
        bucket.set_policy(policy)
        assert(bucket.get_policy() == policy)

def test_bucket_sync_disable():
    zonegroup = realm.master_zonegroup()
    zonegroup_conns = ZonegroupConns(zonegroup)
    buckets, zone_bucket = create_bucket_per_zone(zonegroup_conns)

    for bucket_name in buckets:
        disable_bucket_sync(realm.meta_master_zone(), bucket_name)

    for zone in zonegroup.zones:
        check_buckets_sync_status_obj_not_exist(zone, buckets)

def test_bucket_sync_enable_right_after_disable():
    zonegroup = realm.master_zonegroup()
    zonegroup_conns = ZonegroupConns(zonegroup)
    buckets, zone_bucket = create_bucket_per_zone(zonegroup_conns)

    objnames = ['obj1', 'obj2', 'obj3', 'obj4']
    content = 'asdasd'

    for zone, bucket in zone_bucket:
        for objname in objnames:
            k = new_key(zone, bucket.name, objname)
            k.set_contents_from_string(content)

    for bucket_name in buckets:
        zonegroup_bucket_checkpoint(zonegroup_conns, bucket_name)

    for bucket_name in buckets:
        disable_bucket_sync(realm.meta_master_zone(), bucket_name)
        enable_bucket_sync(realm.meta_master_zone(), bucket_name)

    objnames_2 = ['obj5', 'obj6', 'obj7', 'obj8']

    for zone, bucket in zone_bucket:
        for objname in objnames_2:
            k = new_key(zone, bucket.name, objname)
            k.set_contents_from_string(content)

    for bucket_name in buckets:
        zonegroup_bucket_checkpoint(zonegroup_conns, bucket_name)

def test_bucket_sync_disable_enable():
    zonegroup = realm.master_zonegroup()
    zonegroup_conns = ZonegroupConns(zonegroup)
    buckets, zone_bucket = create_bucket_per_zone(zonegroup_conns)

    objnames = [ 'obj1', 'obj2', 'obj3', 'obj4' ]
    content = 'asdasd'

    for zone, bucket in zone_bucket:
        for objname in objnames:
            k = new_key(zone, bucket.name, objname)
            k.set_contents_from_string(content)

    zonegroup_meta_checkpoint(zonegroup)

    for bucket_name in buckets:
        zonegroup_bucket_checkpoint(zonegroup_conns, bucket_name)

    for bucket_name in buckets:
        disable_bucket_sync(realm.meta_master_zone(), bucket_name)

    zonegroup_meta_checkpoint(zonegroup)

    objnames_2 = [ 'obj5', 'obj6', 'obj7', 'obj8' ]

    for zone, bucket in zone_bucket:
        for objname in objnames_2:
            k = new_key(zone, bucket.name, objname)
            k.set_contents_from_string(content)

    for bucket_name in buckets:
        enable_bucket_sync(realm.meta_master_zone(), bucket_name)

    for bucket_name in buckets:
        zonegroup_bucket_checkpoint(zonegroup_conns, bucket_name)

def test_multipart_object_sync():
    zonegroup = realm.master_zonegroup()
    zonegroup_conns = ZonegroupConns(zonegroup)
    buckets, zone_bucket = create_bucket_per_zone(zonegroup_conns)

    _, bucket = zone_bucket[0]

    # initiate a multipart upload
    upload = bucket.initiate_multipart_upload('MULTIPART')
    mp = boto.s3.multipart.MultiPartUpload(bucket)
    mp.key_name = upload.key_name
    mp.id = upload.id
    part_size = 5 * 1024 * 1024 # 5M min part size
    mp.upload_part_from_file(StringIO('a' * part_size), 1)
    mp.upload_part_from_file(StringIO('b' * part_size), 2)
    mp.upload_part_from_file(StringIO('c' * part_size), 3)
    mp.upload_part_from_file(StringIO('d' * part_size), 4)
    mp.complete_upload()

    zonegroup_bucket_checkpoint(zonegroup_conns, bucket.name)

def test_encrypted_object_sync():
    zonegroup = realm.master_zonegroup()
    zonegroup_conns = ZonegroupConns(zonegroup)

    (zone1, zone2) = zonegroup_conns.rw_zones[0:2]

    # create a bucket on the first zone
    bucket_name = gen_bucket_name()
    log.info('create bucket zone=%s name=%s', zone1.name, bucket_name)
    bucket = zone1.conn.create_bucket(bucket_name)

    # upload an object with sse-c encryption
    sse_c_headers = {
        'x-amz-server-side-encryption-customer-algorithm': 'AES256',
        'x-amz-server-side-encryption-customer-key': 'pO3upElrwuEXSoFwCfnZPdSsmt/xWeFa0N9KgDijwVs=',
        'x-amz-server-side-encryption-customer-key-md5': 'DWygnHRtgiJ77HCm+1rvHw=='
    }
    key = bucket.new_key('testobj-sse-c')
    data = 'A'*512
    key.set_contents_from_string(data, headers=sse_c_headers)

    # upload an object with sse-kms encryption
    sse_kms_headers = {
        'x-amz-server-side-encryption': 'aws:kms',
        # testkey-1 must be present in 'rgw crypt s3 kms encryption keys' (vstart.sh adds this)
        'x-amz-server-side-encryption-aws-kms-key-id': 'testkey-1',
    }
    key = bucket.new_key('testobj-sse-kms')
    key.set_contents_from_string(data, headers=sse_kms_headers)

    # wait for the bucket metadata and data to sync
    zonegroup_meta_checkpoint(zonegroup)
    zone_bucket_checkpoint(zone2.zone, zone1.zone, bucket_name)

    # read the encrypted objects from the second zone
    bucket2 = get_bucket(zone2, bucket_name)
    key = bucket2.get_key('testobj-sse-c', headers=sse_c_headers)
    eq(data, key.get_contents_as_string(headers=sse_c_headers))

    key = bucket2.get_key('testobj-sse-kms')
    eq(data, key.get_contents_as_string())

def test_bucket_index_log_trim():
    zonegroup = realm.master_zonegroup()
    zonegroup_conns = ZonegroupConns(zonegroup)

    zone = zonegroup_conns.rw_zones[0]

    # create a test bucket, upload some objects, and wait for sync
    def make_test_bucket():
        name = gen_bucket_name()
        log.info('create bucket zone=%s name=%s', zone.name, name)
        bucket = zone.conn.create_bucket(name)
        for objname in ('a', 'b', 'c', 'd'):
            k = new_key(zone, name, objname)
            k.set_contents_from_string('foo')
        zonegroup_meta_checkpoint(zonegroup)
        zonegroup_bucket_checkpoint(zonegroup_conns, name)
        return bucket

    # create a 'cold' bucket
    cold_bucket = make_test_bucket()

    # trim with max-buckets=0 to clear counters for cold bucket. this should
    # prevent it from being considered 'active' by the next autotrim
    bilog_autotrim(zone.zone, [
        '--rgw-sync-log-trim-max-buckets', '0',
    ])

    # create an 'active' bucket
    active_bucket = make_test_bucket()

    # trim with max-buckets=1 min-cold-buckets=0 to trim active bucket only
    bilog_autotrim(zone.zone, [
        '--rgw-sync-log-trim-max-buckets', '1',
        '--rgw-sync-log-trim-min-cold-buckets', '0',
    ])

    # verify active bucket has empty bilog
    active_bilog = bilog_list(zone.zone, active_bucket.name)
    assert(len(active_bilog) == 0)

    # verify cold bucket has nonempty bilog
    cold_bilog = bilog_list(zone.zone, cold_bucket.name)
    assert(len(cold_bilog) > 0)

    # trim with min-cold-buckets=999 to trim all buckets
    bilog_autotrim(zone.zone, [
        '--rgw-sync-log-trim-max-buckets', '999',
        '--rgw-sync-log-trim-min-cold-buckets', '999',
    ])

    # verify cold bucket has empty bilog
    cold_bilog = bilog_list(zone.zone, cold_bucket.name)
    assert(len(cold_bilog) == 0)
