  $ radosgw-admin --help
  usage: radosgw-admin <cmd> [options...]
  commands:
    user create                create a new user
    user modify                modify user
    user info                  get user info
    user rm                    remove user
    user suspend               suspend a user
    user enable                re-enable user after suspension
    user check                 check user info
    user stats                 show user stats as accounted by quota subsystem
    user list                  list users
    caps add                   add user capabilities
    caps rm                    remove user capabilities
    subuser create             create a new subuser
    subuser modify             modify subuser
    subuser rm                 remove subuser
    key create                 create access key
    key rm                     remove access key
    bucket list                list buckets
    bucket limit check         show bucket sharding stats
    bucket link                link bucket to specified user
    bucket unlink              unlink bucket from specified user
    bucket stats               returns bucket statistics
    bucket rm                  remove bucket
    bucket check               check bucket index
    bucket reshard             reshard bucket
    bucket sync disable        disable bucket sync
    bucket sync enable         enable bucket sync
    bi get                     retrieve bucket index object entries
    bi put                     store bucket index object entries
    bi list                    list raw bucket index entries
    object rm                  remove object
    object stat                stat an object for its metadata
    object unlink              unlink object from bucket index
    objects expire             run expired objects cleanup
    period delete              delete a period
    period get                 get period info
    period get-current         get current period info
    period pull                pull a period
    period push                push a period
    period list                list all periods
    period update              update the staging period
    period commit              commit the staging period
    quota set                  set quota params
    quota enable               enable quota
    quota disable              disable quota
    global quota get           view global quota params
    global quota set           set global quota params
    global quota enable        enable a global quota
    global quota disable       disable a global quota
    realm create               create a new realm
    realm delete               delete a realm
    realm get                  show realm info
    realm get-default          get default realm name
    realm list                 list realms
    realm list-periods         list all realm periods
    realm remove               remove a zonegroup from the realm
    realm rename               rename a realm
    realm set                  set realm info (requires infile)
    realm default              set realm as default
    realm pull                 pull a realm and its current period
    zonegroup add              add a zone to a zonegroup
    zonegroup create           create a new zone group info
    zonegroup default          set default zone group
    zonegroup delete           delete a zone group info
    zonegroup get              show zone group info
    zonegroup modify           modify an existing zonegroup
    zonegroup set              set zone group info (requires infile)
    zonegroup remove           remove a zone from a zonegroup
    zonegroup rename           rename a zone group
    zonegroup list             list all zone groups set on this cluster
    zonegroup placement list   list zonegroup's placement targets
    zonegroup placement add    add a placement target id to a zonegroup
    zonegroup placement modify modify a placement target of a specific zonegroup
    zonegroup placement rm     remove a placement target from a zonegroup
    zonegroup placement default  set a zonegroup's default placement target
    zone create                create a new zone
    zone delete                delete a zone
    zone get                   show zone cluster params
    zone modify                modify an existing zone
    zone set                   set zone cluster params (requires infile)
    zone list                  list all zones set on this cluster
    zone rename                rename a zone
    zone placement list        list zone's placement targets
    zone placement add         add a zone placement target
    zone placement modify      modify a zone placement target
    zone placement rm          remove a zone placement target
    pool add                   add an existing pool for data placement
    pool rm                    remove an existing pool from data placement set
    pools list                 list placement active set
    policy                     read bucket/object policy
    log list                   list log objects
    log show                   dump a log from specific object or (bucket + date
                               + bucket-id)
                               (NOTE: required to specify formatting of date
                               to "YYYY-MM-DD-hh")
    log rm                     remove log object
    usage show                 show usage (by user, date range)
    usage trim                 trim usage (by user, date range)
    gc list                    dump expired garbage collection objects (specify
                               --include-all to list all entries, including unexpired)
    gc process                 manually process garbage
    lc list                    list all bucket lifecycle progress
    lc process                 manually process lifecycle
    metadata get               get metadata info
    metadata put               put metadata info
    metadata rm                remove metadata info
    metadata list              list metadata info
    mdlog list                 list metadata log
    mdlog trim                 trim metadata log (use start-date, end-date or
                               start-marker, end-marker)
    mdlog status               read metadata log status
    bilog list                 list bucket index log
    bilog trim                 trim bucket index log (use start-marker, end-marker)
    datalog list               list data log
    datalog trim               trim data log
    datalog status             read data log status
    opstate list               list stateful operations entries (use client_id,
                               op_id, object)
    opstate set                set state on an entry (use client_id, op_id, object, state)
    opstate renew              renew state on an entry (use client_id, op_id, object)
    opstate rm                 remove entry (use client_id, op_id, object)
    replicalog get             get replica metadata log entry
    replicalog update          update replica metadata log entry
    replicalog delete          delete replica metadata log entry
    orphans find               init and run search for leaked rados objects (use job-id, pool)
    orphans finish             clean up search for leaked rados objects
    orphans list-jobs          list the current job-ids for orphans search
    role create                create a AWS role for use with STS
    role delete                delete a role
    role get                   get a role
    role list                  list roles with specified path prefix
    role modify                modify the assume role policy of an existing role
    role-policy put            add/update permission policy to role
    role-policy list           list policies attached to a role
    role-policy get            get the specified inline policy document embedded with the given role
    role-policy delete         delete policy attached to a role
    reshard add                schedule a resharding of a bucket
    reshard list               list all bucket resharding or scheduled to be reshared
    reshard process            process of scheduled reshard jobs
    reshard cancel             cancel resharding a bucket
    sync error list            list sync error
    sync error trim            trim sync error
  options:
     --tenant=<tenant>         tenant name
     --uid=<id>                user id
     --subuser=<name>          subuser name
     --access-key=<key>        S3 access key
     --email=<email>
     --secret/--secret-key=<key>
                               specify secret key
     --gen-access-key          generate random access key (for S3)
     --gen-secret              generate random secret key
     --key-type=<type>         key type, options are: swift, s3
     --temp-url-key[-2]=<key>  temp url key
     --access=<access>         Set access permissions for sub-user, should be one
                               of read, write, readwrite, full
     --display-name=<name>
     --max-buckets             max number of buckets for a user
     --admin                   set the admin flag on the user
     --system                  set the system flag on the user
     --bucket=<bucket>
     --pool=<pool>
     --object=<object>
     --date=<date>
     --start-date=<date>
     --end-date=<date>
     --bucket-id=<bucket-id>
     --shard-id=<shard-id>     optional for mdlog list
                               required for: 
                                 mdlog trim
                                 replica mdlog get/delete
                                 replica datalog get/delete
     --metadata-key=<key>      key to retrieve metadata from with metadata get
     --remote=<remote>         zone or zonegroup id of remote gateway
     --period=<id>             period id
     --epoch=<number>          period epoch
     --commit                  commit the period during 'period update'
     --staging                 get staging period info
     --master                  set as master
     --master-url              master url
     --master-zonegroup=<id>   master zonegroup id
     --master-zone=<id>        master zone id
     --rgw-realm=<name>        realm name
     --realm-id=<id>           realm id
     --realm-new-name=<name>   realm new name
     --rgw-zonegroup=<name>    zonegroup name
     --zonegroup-id=<id>       zonegroup id
     --zonegroup-new-name=<name>
                               zonegroup new name
     --rgw-zone=<name>         name of zone in which radosgw is running
     --zone-id=<id>            zone id
     --zone-new-name=<name>    zone new name
     --source-zone             specify the source zone (for data sync)
     --default                 set entity (realm, zonegroup, zone) as default
     --read-only               set zone as read-only (when adding to zonegroup)
     --placement-id            placement id for zonegroup placement commands
     --tags=<list>             list of tags for zonegroup placement add and modify commands
     --tags-add=<list>         list of tags to add for zonegroup placement modify command
     --tags-rm=<list>          list of tags to remove for zonegroup placement modify command
     --endpoints=<list>        zone endpoints
     --index-pool=<pool>       placement target index pool
     --data-pool=<pool>        placement target data pool
     --data-extra-pool=<pool>  placement target data extra (non-ec) pool
     --placement-index-type=<type>
                               placement target index type (normal, indexless, or #id)
     --compression=<type>      placement target compression type (plugin name or empty/none)
     --tier-type=<type>        zone tier type
     --tier-config=<k>=<v>[,...]
                               set zone tier config keys, values
     --tier-config-rm=<k>[,...]
                               unset zone tier config keys
     --sync-from-all[=false]   set/reset whether zone syncs from all zonegroup peers
     --sync-from=[zone-name][,...]
                               set list of zones to sync from
     --sync-from-rm=[zone-name][,...]
                               remove zones from list of zones to sync from
     --fix                     besides checking bucket index, will also fix it
     --check-objects           bucket check: rebuilds bucket index according to
                               actual objects state
     --format=<format>         specify output format for certain operations: xml,
                               json
     --purge-data              when specified, user removal will also purge all the
                               user data
     --purge-keys              when specified, subuser removal will also purge all the
                               subuser keys
     --purge-objects           remove a bucket's objects before deleting it
                               (NOTE: required to delete a non-empty bucket)
     --sync-stats              option to 'user stats', update user stats with current
                               stats reported by user's buckets indexes
     --reset-stats             option to 'user stats', reset stats in accordance with user buckets
     --show-log-entries=<flag> enable/disable dump of log entries on log show
     --show-log-sum=<flag>     enable/disable dump of log summation on log show
     --skip-zero-entries       log show only dumps entries that don't have zero value
                               in one of the numeric field
     --infile=<file>           specify a file to read in when setting data
     --state=<state string>    specify a state for the opstate set command
     --replica-log-type        replica log type (metadata, data, bucket), required for
                               replica log operations
     --categories=<list>       comma separated list of categories, used in usage show
     --caps=<caps>             list of caps (e.g., "usage=read, write; user=read")
     --yes-i-really-mean-it    required for certain operations
     --warnings-only           when specified with bucket limit check, list
                               only buckets nearing or over the current max
                               objects per shard value
     --bypass-gc               when specified with bucket deletion, triggers
                               object deletions by not involving GC
     --inconsistent-index      when specified with bucket deletion and bypass-gc set to true,
                               ignores bucket index consistency
  
  <date> := "YYYY-MM-DD[ hh:mm:ss]"
  
  Quota options:
     --bucket                  specified bucket for quota command
     --max-objects             specify max objects (negative value to disable)
     --max-size                specify max size (in B/K/M/G/T, negative value to disable)
     --quota-scope             scope of quota (bucket, user)
  
  Orphans search options:
     --pool                    data pool to scan for leaked rados objects in
     --num-shards              num of shards to use for keeping the temporary scan info
     --orphan-stale-secs       num of seconds to wait before declaring an object to be an orphan (default: 86400)
     --job-id                  set the job id (for orphans find)
     --max-concurrent-ios      maximum concurrent ios for orphans find (default: 32)
  
  Orphans list-jobs options:
     --extra-info              provide extra info in job list
  
  Role options:
     --role-name               name of the role to create
     --path                    path to the role
     --assume-role-policy-doc  the trust relationship policy document that grants an entity permission to assume the role
     --policy-name             name of the policy document
     --policy-doc              permission policy document
     --path-prefix             path prefix for filtering roles
  
    --conf/-c FILE    read configuration from the given configuration file
    --id ID           set ID portion of my name
    --name/-n TYPE.ID set name
    --cluster NAME    set cluster name (default: ceph)
    --setuser USER    set uid to user or uid (and gid to user's gid)
    --setgroup GROUP  set gid to group or gid
    --version         show version and quit
  
  [1]


