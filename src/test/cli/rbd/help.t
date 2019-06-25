Skip test on FreeBSD as it generates different output there.

  $ test "$(uname)" = "FreeBSD" && exit 80 || true

  $ rbd --help
  usage: rbd <command> ...
  
  Command-line interface for managing Ceph RBD images.
  
  Positional arguments:
    <command>
      bench                       Simple benchmark.
      children                    Display children of snapshot.
      clone                       Clone a snapshot into a COW child image.
      copy (cp)                   Copy src image to dest.
      create                      Create an empty image.
      diff                        Print extents that differ since a previous
                                  snap, or image creation.
      disk-usage (du)             Show disk usage stats for pool, image or
                                  snapshot
      export                      Export image to file.
      export-diff                 Export incremental diff to file.
      feature disable             Disable the specified image feature.
      feature enable              Enable the specified image feature.
      flatten                     Fill clone with parent data (make it
                                  independent).
      image-meta get              Image metadata get the value associated with
                                  the key.
      image-meta list             Image metadata list keys with values.
      image-meta remove           Image metadata remove the key and value
                                  associated.
      image-meta set              Image metadata set key with value.
      import                      Import image from file.
      import-diff                 Import an incremental diff.
      info                        Show information about image size, striping,
                                  etc.
      journal client disconnect   Flag image journal client as disconnected.
      journal export              Export image journal.
      journal import              Import image journal.
      journal info                Show information about image journal.
      journal inspect             Inspect image journal for structural errors.
      journal reset               Reset image journal.
      journal status              Show status of image journal.
      list (ls)                   List rbd images.
      lock add                    Take a lock on an image.
      lock list (lock ls)         Show locks held on an image.
      lock remove (lock rm)       Release a lock on an image.
      map                         Map image to a block device using the kernel.
      merge-diff                  Merge two diff exports together.
      mirror image demote         Demote an image to non-primary for RBD
                                  mirroring.
      mirror image disable        Disable RBD mirroring for an image.
      mirror image enable         Enable RBD mirroring for an image.
      mirror image promote        Promote an image to primary for RBD mirroring.
      mirror image resync         Force resync to primary image for RBD mirroring.
      mirror image status         Show RDB mirroring status for an image.
      mirror pool demote          Demote all primary images in the pool.
      mirror pool disable         Disable RBD mirroring by default within a pool.
      mirror pool enable          Enable RBD mirroring by default within a pool.
      mirror pool info            Show information about the pool mirroring
                                  configuration.
      mirror pool peer add        Add a mirroring peer to a pool.
      mirror pool peer remove     Remove a mirroring peer from a pool.
      mirror pool peer set        Update mirroring peer settings.
      mirror pool promote         Promote all non-primary images in the pool.
      mirror pool status          Show status for all mirrored images in the pool.
      nbd list (nbd ls)           List the nbd devices already used.
      nbd map                     Map image to a nbd device.
      nbd unmap                   Unmap a nbd device.
      object-map check            Verify the object map is correct.
      object-map rebuild          Rebuild an invalid object map.
      pool init                   Initialize pool for use by RBD.
      remove (rm)                 Delete an image.
      rename (mv)                 Rename image within pool.
      resize                      Resize (expand or shrink) image.
      showmapped                  Show the rbd images mapped by the kernel.
      snap create (snap add)      Create a snapshot.
      snap limit clear            Remove snapshot limit.
      snap limit set              Limit the number of snapshots.
      snap list (snap ls)         Dump list of image snapshots.
      snap protect                Prevent a snapshot from being deleted.
      snap purge                  Delete all snapshots.
      snap remove (snap rm)       Delete a snapshot.
      snap rename                 Rename a snapshot.
      snap rollback (snap revert) Rollback image to snapshot.
      snap unprotect              Allow a snapshot to be deleted.
      status                      Show the status of this image.
      trash list (trash ls)       List trash images.
      trash move (trash mv)       Move an image to the trash.
      trash remove (trash rm)     Remove an image from trash.
      trash restore               Restore an image from trash.
      unmap                       Unmap a rbd device that was used by the kernel.
      watch                       Watch events on image.
  
  Optional arguments:
    -c [ --conf ] arg     path to cluster configuration
    --cluster arg         cluster name
    --id arg              client id (without 'client.' prefix)
    --user arg            client id (without 'client.' prefix)
    -n [ --name ] arg     client name
    -m [ --mon_host ] arg monitor host
    --secret arg          path to secret key (deprecated)
    -K [ --keyfile ] arg  path to secret key
    -k [ --keyring ] arg  path to keyring
  
  See 'rbd help <command>' for help on a specific command.
  $ rbd help | grep '^    [a-z]' | sed 's/^    \([a-z -]*[a-z]\).*/\1/g' | while read -r line; do echo rbd help $line ; rbd help $line; done
  rbd help bench
  usage: rbd bench [--pool <pool>] [--image <image>] [--io-size <io-size>] 
                   [--io-threads <io-threads>] [--io-total <io-total>] 
                   [--io-pattern <io-pattern>] --io-type <io-type> 
                   <image-spec> 
  
  Simple benchmark.
  
  Positional arguments
    <image-spec>         image specification
                         (example: [<pool-name>/]<image-name>)
  
  Optional arguments
    -p [ --pool ] arg    pool name
    --image arg          image name
    --io-size arg        IO size (in B/K/M/G/T) [default: 4K]
    --io-threads arg     ios in flight [default: 16]
    --io-total arg       total size for IO (in B/K/M/G/T) [default: 1G]
    --io-pattern arg     IO pattern (rand or seq) [default: seq]
    --io-type arg        IO type (read or write)
  
  rbd help children
  usage: rbd children [--pool <pool>] [--image <image>] [--snap <snap>] 
                      [--format <format>] [--pretty-format] 
                      <snap-spec> 
  
  Display children of snapshot.
  
  Positional arguments
    <snap-spec>          snapshot specification
                         (example: [<pool-name>/]<image-name>@<snapshot-name>)
  
  Optional arguments
    -p [ --pool ] arg    pool name
    --image arg          image name
    --snap arg           snapshot name
    --format arg         output format (plain, json, or xml) [default: plain]
    --pretty-format      pretty formatting (json and xml)
  
  rbd help clone
  usage: rbd clone [--pool <pool>] [--image <image>] [--snap <snap>] 
                   [--dest-pool <dest-pool>] [--dest <dest>] [--order <order>] 
                   [--object-size <object-size>] 
                   [--image-feature <image-feature>] [--image-shared] 
                   [--stripe-unit <stripe-unit>] [--stripe-count <stripe-count>] 
                   [--data-pool <data-pool>] 
                   [--journal-splay-width <journal-splay-width>] 
                   [--journal-object-size <journal-object-size>] 
                   [--journal-pool <journal-pool>] 
                   <source-snap-spec> <dest-image-spec> 
  
  Clone a snapshot into a COW child image.
  
  Positional arguments
    <source-snap-spec>        source snapshot specification
                              (example:
                              [<pool-name>/]<image-name>@<snapshot-name>)
    <dest-image-spec>         destination image specification
                              (example: [<pool-name>/]<image-name>)
  
  Optional arguments
    -p [ --pool ] arg         source pool name
    --image arg               source image name
    --snap arg                source snapshot name
    --dest-pool arg           destination pool name
    --dest arg                destination image name
    --order arg               object order [12 <= order <= 25]
    --object-size arg         object size in B/K/M [4K <= object size <= 32M]
    --image-feature arg       image features
                              [layering(+), striping, exclusive-lock(+*),
                              object-map(+*), fast-diff(+*), deep-flatten(+-),
                              journaling(*), data-pool]
    --image-shared            shared image
    --stripe-unit arg         stripe unit in B/K/M
    --stripe-count arg        stripe count
    --data-pool arg           data pool
    --journal-splay-width arg number of active journal objects
    --journal-object-size arg size of journal objects
    --journal-pool arg        pool for journal objects
  
  Image Features:
    (*) supports enabling/disabling on existing images
    (-) supports disabling-only on existing images
    (+) enabled by default for new images if features not specified
  
  rbd help copy
  usage: rbd copy [--pool <pool>] [--image <image>] [--snap <snap>] 
                  [--dest-pool <dest-pool>] [--dest <dest>] [--order <order>] 
                  [--object-size <object-size>] 
                  [--image-feature <image-feature>] [--image-shared] 
                  [--stripe-unit <stripe-unit>] [--stripe-count <stripe-count>] 
                  [--data-pool <data-pool>] 
                  [--journal-splay-width <journal-splay-width>] 
                  [--journal-object-size <journal-object-size>] 
                  [--journal-pool <journal-pool>] [--sparse-size <sparse-size>] 
                  [--no-progress] 
                  <source-image-or-snap-spec> <dest-image-spec> 
  
  Copy src image to dest.
  
  Positional arguments
    <source-image-or-snap-spec>  source image or snapshot specification
                                 (example:
                                 [<pool-name>/]<image-name>[@<snap-name>])
    <dest-image-spec>            destination image specification
                                 (example: [<pool-name>/]<image-name>)
  
  Optional arguments
    -p [ --pool ] arg            source pool name
    --image arg                  source image name
    --snap arg                   source snapshot name
    --dest-pool arg              destination pool name
    --dest arg                   destination image name
    --order arg                  object order [12 <= order <= 25]
    --object-size arg            object size in B/K/M [4K <= object size <= 32M]
    --image-feature arg          image features
                                 [layering(+), striping, exclusive-lock(+*),
                                 object-map(+*), fast-diff(+*), deep-flatten(+-),
                                 journaling(*), data-pool]
    --image-shared               shared image
    --stripe-unit arg            stripe unit in B/K/M
    --stripe-count arg           stripe count
    --data-pool arg              data pool
    --journal-splay-width arg    number of active journal objects
    --journal-object-size arg    size of journal objects
    --journal-pool arg           pool for journal objects
    --sparse-size arg            sparse size in B/K/M [default: 4K]
    --no-progress                disable progress output
  
  Image Features:
    (*) supports enabling/disabling on existing images
    (-) supports disabling-only on existing images
    (+) enabled by default for new images if features not specified
  
  rbd help create
  usage: rbd create [--pool <pool>] [--image <image>] 
                    [--image-format <image-format>] [--new-format] 
                    [--order <order>] [--object-size <object-size>] 
                    [--image-feature <image-feature>] [--image-shared] 
                    [--stripe-unit <stripe-unit>] 
                    [--stripe-count <stripe-count>] [--data-pool <data-pool>] 
                    [--journal-splay-width <journal-splay-width>] 
                    [--journal-object-size <journal-object-size>] 
                    [--journal-pool <journal-pool>] --size <size> 
                    <image-spec> 
  
  Create an empty image.
  
  Positional arguments
    <image-spec>              image specification
                              (example: [<pool-name>/]<image-name>)
  
  Optional arguments
    -p [ --pool ] arg         pool name
    --image arg               image name
    --image-format arg        image format [1 (deprecated) or 2]
    --new-format              use image format 2
                              (deprecated)
    --order arg               object order [12 <= order <= 25]
    --object-size arg         object size in B/K/M [4K <= object size <= 32M]
    --image-feature arg       image features
                              [layering(+), striping, exclusive-lock(+*),
                              object-map(+*), fast-diff(+*), deep-flatten(+-),
                              journaling(*), data-pool]
    --image-shared            shared image
    --stripe-unit arg         stripe unit in B/K/M
    --stripe-count arg        stripe count
    --data-pool arg           data pool
    --journal-splay-width arg number of active journal objects
    --journal-object-size arg size of journal objects
    --journal-pool arg        pool for journal objects
    -s [ --size ] arg         image size (in M/G/T) [default: M]
  
  Image Features:
    (*) supports enabling/disabling on existing images
    (-) supports disabling-only on existing images
    (+) enabled by default for new images if features not specified
  
  rbd help diff
  usage: rbd diff [--pool <pool>] [--image <image>] [--snap <snap>] 
                  [--from-snap <from-snap>] [--whole-object] [--format <format>] 
                  [--pretty-format] 
                  <image-or-snap-spec> 
  
  Print extents that differ since a previous snap, or image creation.
  
  Positional arguments
    <image-or-snap-spec>  image or snapshot specification
                          (example: [<pool-name>/]<image-name>[@<snap-name>])
  
  Optional arguments
    -p [ --pool ] arg     pool name
    --image arg           image name
    --snap arg            snapshot name
    --from-snap arg       snapshot starting point
    --whole-object        compare whole object
    --format arg          output format (plain, json, or xml) [default: plain]
    --pretty-format       pretty formatting (json and xml)
  
  rbd help disk-usage
  usage: rbd disk-usage [--pool <pool>] [--image <image>] [--snap <snap>] 
                        [--format <format>] [--pretty-format] 
                        [--from-snap <from-snap>] 
                        <image-or-snap-spec> 
  
  Show disk usage stats for pool, image or snapshot
  
  Positional arguments
    <image-or-snap-spec>  image or snapshot specification
                          (example: [<pool-name>/]<image-name>[@<snap-name>])
  
  Optional arguments
    -p [ --pool ] arg     pool name
    --image arg           image name
    --snap arg            snapshot name
    --format arg          output format (plain, json, or xml) [default: plain]
    --pretty-format       pretty formatting (json and xml)
    --from-snap arg       snapshot starting point
  
  rbd help export
  usage: rbd export [--pool <pool>] [--image <image>] [--snap <snap>] 
                    [--path <path>] [--no-progress] 
                    [--export-format <export-format>] 
                    <source-image-or-snap-spec> <path-name> 
  
  Export image to file.
  
  Positional arguments
    <source-image-or-snap-spec>  source image or snapshot specification
                                 (example:
                                 [<pool-name>/]<image-name>[@<snap-name>])
    <path-name>                  export file (or '-' for stdout)
  
  Optional arguments
    -p [ --pool ] arg            source pool name
    --image arg                  source image name
    --snap arg                   source snapshot name
    --path arg                   export file (or '-' for stdout)
    --no-progress                disable progress output
    --export-format arg          format of image file
  
  rbd help export-diff
  usage: rbd export-diff [--pool <pool>] [--image <image>] [--snap <snap>] 
                         [--path <path>] [--from-snap <from-snap>] 
                         [--whole-object] [--no-progress] 
                         <source-image-or-snap-spec> <path-name> 
  
  Export incremental diff to file.
  
  Positional arguments
    <source-image-or-snap-spec>  source image or snapshot specification
                                 (example:
                                 [<pool-name>/]<image-name>[@<snap-name>])
    <path-name>                  export file (or '-' for stdout)
  
  Optional arguments
    -p [ --pool ] arg            source pool name
    --image arg                  source image name
    --snap arg                   source snapshot name
    --path arg                   export file (or '-' for stdout)
    --from-snap arg              snapshot starting point
    --whole-object               compare whole object
    --no-progress                disable progress output
  
  rbd help feature disable
  usage: rbd feature disable [--pool <pool>] [--image <image>] 
                             <image-spec> <features> [<features> ...]
  
  Disable the specified image feature.
  
  Positional arguments
    <image-spec>         image specification
                         (example: [<pool-name>/]<image-name>)
    <features>           image features
                         [layering, striping, exclusive-lock, object-map,
                         fast-diff, deep-flatten, journaling, data-pool]
  
  Optional arguments
    -p [ --pool ] arg    pool name
    --image arg          image name
  
  rbd help feature enable
  usage: rbd feature enable [--pool <pool>] [--image <image>] 
                            [--journal-splay-width <journal-splay-width>] 
                            [--journal-object-size <journal-object-size>] 
                            [--journal-pool <journal-pool>] 
                            <image-spec> <features> [<features> ...]
  
  Enable the specified image feature.
  
  Positional arguments
    <image-spec>              image specification
                              (example: [<pool-name>/]<image-name>)
    <features>                image features
                              [layering, striping, exclusive-lock, object-map,
                              fast-diff, deep-flatten, journaling, data-pool]
  
  Optional arguments
    -p [ --pool ] arg         pool name
    --image arg               image name
    --journal-splay-width arg number of active journal objects
    --journal-object-size arg size of journal objects
    --journal-pool arg        pool for journal objects
  
  rbd help flatten
  usage: rbd flatten [--pool <pool>] [--image <image>] [--no-progress] 
                     <image-spec> 
  
  Fill clone with parent data (make it independent).
  
  Positional arguments
    <image-spec>         image specification
                         (example: [<pool-name>/]<image-name>)
  
  Optional arguments
    -p [ --pool ] arg    pool name
    --image arg          image name
    --no-progress        disable progress output
  
  rbd help image-meta get
  usage: rbd image-meta get [--pool <pool>] [--image <image>] 
                            <image-spec> <key> 
  
  Image metadata get the value associated with the key.
  
  Positional arguments
    <image-spec>         image specification
                         (example: [<pool-name>/]<image-name>)
    <key>                image meta key
  
  Optional arguments
    -p [ --pool ] arg    pool name
    --image arg          image name
  
  rbd help image-meta list
  usage: rbd image-meta list [--pool <pool>] [--image <image>] 
                             [--format <format>] [--pretty-format] 
                             <image-spec> 
  
  Image metadata list keys with values.
  
  Positional arguments
    <image-spec>         image specification
                         (example: [<pool-name>/]<image-name>)
  
  Optional arguments
    -p [ --pool ] arg    pool name
    --image arg          image name
    --format arg         output format (plain, json, or xml) [default: plain]
    --pretty-format      pretty formatting (json and xml)
  
  rbd help image-meta remove
  usage: rbd image-meta remove [--pool <pool>] [--image <image>] 
                               <image-spec> <key> 
  
  Image metadata remove the key and value associated.
  
  Positional arguments
    <image-spec>         image specification
                         (example: [<pool-name>/]<image-name>)
    <key>                image meta key
  
  Optional arguments
    -p [ --pool ] arg    pool name
    --image arg          image name
  
  rbd help image-meta set
  usage: rbd image-meta set [--pool <pool>] [--image <image>] 
                            <image-spec> <key> <value> 
  
  Image metadata set key with value.
  
  Positional arguments
    <image-spec>         image specification
                         (example: [<pool-name>/]<image-name>)
    <key>                image meta key
    <value>              image meta value
  
  Optional arguments
    -p [ --pool ] arg    pool name
    --image arg          image name
  
  rbd help import
  usage: rbd import [--path <path>] [--dest-pool <dest-pool>] [--dest <dest>] 
                    [--image-format <image-format>] [--new-format] 
                    [--order <order>] [--object-size <object-size>] 
                    [--image-feature <image-feature>] [--image-shared] 
                    [--stripe-unit <stripe-unit>] 
                    [--stripe-count <stripe-count>] [--data-pool <data-pool>] 
                    [--journal-splay-width <journal-splay-width>] 
                    [--journal-object-size <journal-object-size>] 
                    [--journal-pool <journal-pool>] 
                    [--sparse-size <sparse-size>] [--no-progress] 
                    [--export-format <export-format>] [--pool <pool>] 
                    [--image <image>] 
                    <path-name> <dest-image-spec> 
  
  Import image from file.
  
  Positional arguments
    <path-name>               import file (or '-' for stdin)
    <dest-image-spec>         destination image specification
                              (example: [<pool-name>/]<image-name>)
  
  Optional arguments
    --path arg                import file (or '-' for stdin)
    --dest-pool arg           destination pool name
    --dest arg                destination image name
    --image-format arg        image format [1 (deprecated) or 2]
    --new-format              use image format 2
                              (deprecated)
    --order arg               object order [12 <= order <= 25]
    --object-size arg         object size in B/K/M [4K <= object size <= 32M]
    --image-feature arg       image features
                              [layering(+), striping, exclusive-lock(+*),
                              object-map(+*), fast-diff(+*), deep-flatten(+-),
                              journaling(*), data-pool]
    --image-shared            shared image
    --stripe-unit arg         stripe unit in B/K/M
    --stripe-count arg        stripe count
    --data-pool arg           data pool
    --journal-splay-width arg number of active journal objects
    --journal-object-size arg size of journal objects
    --journal-pool arg        pool for journal objects
    --sparse-size arg         sparse size in B/K/M [default: 4K]
    --no-progress             disable progress output
    --export-format arg       format of image file
    -p [ --pool ] arg         pool name (deprecated)
    --image arg               image name (deprecated)
  
  Image Features:
    (*) supports enabling/disabling on existing images
    (-) supports disabling-only on existing images
    (+) enabled by default for new images if features not specified
  
  rbd help import-diff
  usage: rbd import-diff [--path <path>] [--pool <pool>] [--image <image>] 
                         [--sparse-size <sparse-size>] [--no-progress] 
                         <path-name> <image-spec> 
  
  Import an incremental diff.
  
  Positional arguments
    <path-name>          import file (or '-' for stdin)
    <image-spec>         image specification
                         (example: [<pool-name>/]<image-name>)
  
  Optional arguments
    --path arg           import file (or '-' for stdin)
    -p [ --pool ] arg    pool name
    --image arg          image name
    --sparse-size arg    sparse size in B/K/M [default: 4K]
    --no-progress        disable progress output
  
  rbd help info
  usage: rbd info [--pool <pool>] [--image <image>] [--snap <snap>] 
                  [--image-id <image-id>] [--format <format>] [--pretty-format] 
                  <image-or-snap-spec> 
  
  Show information about image size, striping, etc.
  
  Positional arguments
    <image-or-snap-spec>  image or snapshot specification
                          (example: [<pool-name>/]<image-name>[@<snap-name>])
  
  Optional arguments
    -p [ --pool ] arg     pool name
    --image arg           image name
    --snap arg            snapshot name
    --image-id arg        image id
    --format arg          output format (plain, json, or xml) [default: plain]
    --pretty-format       pretty formatting (json and xml)
  
  rbd help journal client disconnect
  usage: rbd journal client disconnect [--pool <pool>] [--image <image>] 
                                       [--journal <journal>] 
                                       [--client-id <client-id>] 
                                       <journal-spec> 
  
  Flag image journal client as disconnected.
  
  Positional arguments
    <journal-spec>       journal specification
                         (example: [<pool-name>/]<journal-name>)
  
  Optional arguments
    -p [ --pool ] arg    pool name
    --image arg          image name
    --journal arg        journal name
    --client-id arg      client ID (or leave unspecified to disconnect all)
  
  rbd help journal export
  usage: rbd journal export [--pool <pool>] [--image <image>] 
                            [--journal <journal>] [--path <path>] [--verbose] 
                            [--no-error] 
                            <source-journal-spec> <path-name> 
  
  Export image journal.
  
  Positional arguments
    <source-journal-spec>  source journal specification
                           (example: [<pool-name>/]<journal-name>)
    <path-name>            export file (or '-' for stdout)
  
  Optional arguments
    -p [ --pool ] arg      source pool name
    --image arg            source image name
    --journal arg          source journal name
    --path arg             export file (or '-' for stdout)
    --verbose              be verbose
    --no-error             continue after error
  
  rbd help journal import
  usage: rbd journal import [--path <path>] [--dest-pool <dest-pool>] 
                            [--dest <dest>] [--dest-journal <dest-journal>] 
                            [--verbose] [--no-error] 
                            <path-name> <dest-journal-spec> 
  
  Import image journal.
  
  Positional arguments
    <path-name>          import file (or '-' for stdin)
    <dest-journal-spec>  destination journal specification
                         (example: [<pool-name>/]<journal-name>)
  
  Optional arguments
    --path arg           import file (or '-' for stdin)
    --dest-pool arg      destination pool name
    --dest arg           destination image name
    --dest-journal arg   destination journal name
    --verbose            be verbose
    --no-error           continue after error
  
  rbd help journal info
  usage: rbd journal info [--pool <pool>] [--image <image>] 
                          [--journal <journal>] [--format <format>] 
                          [--pretty-format] 
                          <journal-spec> 
  
  Show information about image journal.
  
  Positional arguments
    <journal-spec>       journal specification
                         (example: [<pool-name>/]<journal-name>)
  
  Optional arguments
    -p [ --pool ] arg    pool name
    --image arg          image name
    --journal arg        journal name
    --format arg         output format (plain, json, or xml) [default: plain]
    --pretty-format      pretty formatting (json and xml)
  
  rbd help journal inspect
  usage: rbd journal inspect [--pool <pool>] [--image <image>] 
                             [--journal <journal>] [--verbose] 
                             <journal-spec> 
  
  Inspect image journal for structural errors.
  
  Positional arguments
    <journal-spec>       journal specification
                         (example: [<pool-name>/]<journal-name>)
  
  Optional arguments
    -p [ --pool ] arg    pool name
    --image arg          image name
    --journal arg        journal name
    --verbose            be verbose
  
  rbd help journal reset
  usage: rbd journal reset [--pool <pool>] [--image <image>] 
                           [--journal <journal>] 
                           <journal-spec> 
  
  Reset image journal.
  
  Positional arguments
    <journal-spec>       journal specification
                         (example: [<pool-name>/]<journal-name>)
  
  Optional arguments
    -p [ --pool ] arg    pool name
    --image arg          image name
    --journal arg        journal name
  
  rbd help journal status
  usage: rbd journal status [--pool <pool>] [--image <image>] 
                            [--journal <journal>] [--format <format>] 
                            [--pretty-format] 
                            <journal-spec> 
  
  Show status of image journal.
  
  Positional arguments
    <journal-spec>       journal specification
                         (example: [<pool-name>/]<journal-name>)
  
  Optional arguments
    -p [ --pool ] arg    pool name
    --image arg          image name
    --journal arg        journal name
    --format arg         output format (plain, json, or xml) [default: plain]
    --pretty-format      pretty formatting (json and xml)
  
  rbd help list
  usage: rbd list [--long] [--pool <pool>] [--format <format>] [--pretty-format] 
                  <pool-name> 
  
  List rbd images.
  
  Positional arguments
    <pool-name>          pool name
  
  Optional arguments
    -l [ --long ]        long listing format
    -p [ --pool ] arg    pool name
    --format arg         output format (plain, json, or xml) [default: plain]
    --pretty-format      pretty formatting (json and xml)
  
  rbd help lock add
  usage: rbd lock add [--pool <pool>] [--image <image>] [--shared <shared>] 
                      <image-spec> <lock-id> 
  
  Take a lock on an image.
  
  Positional arguments
    <image-spec>         image specification
                         (example: [<pool-name>/]<image-name>)
    <lock-id>            unique lock id
  
  Optional arguments
    -p [ --pool ] arg    pool name
    --image arg          image name
    --shared arg         shared lock tag
  
  rbd help lock list
  usage: rbd lock list [--pool <pool>] [--image <image>] [--format <format>] 
                       [--pretty-format] 
                       <image-spec> 
  
  Show locks held on an image.
  
  Positional arguments
    <image-spec>         image specification
                         (example: [<pool-name>/]<image-name>)
  
  Optional arguments
    -p [ --pool ] arg    pool name
    --image arg          image name
    --format arg         output format (plain, json, or xml) [default: plain]
    --pretty-format      pretty formatting (json and xml)
  
  rbd help lock remove
  usage: rbd lock remove [--pool <pool>] [--image <image>] 
                         <image-spec> <lock-id> <locker> 
  
  Release a lock on an image.
  
  Positional arguments
    <image-spec>         image specification
                         (example: [<pool-name>/]<image-name>)
    <lock-id>            unique lock id
    <locker>             locker client
  
  Optional arguments
    -p [ --pool ] arg    pool name
    --image arg          image name
  
  rbd help map
  usage: rbd map [--pool <pool>] [--image <image>] [--snap <snap>] 
                 [--options <options>] [--read-only] [--exclusive] 
                 <image-or-snap-spec> 
  
  Map image to a block device using the kernel.
  
  Positional arguments
    <image-or-snap-spec>  image or snapshot specification
                          (example: [<pool-name>/]<image-name>[@<snap-name>])
  
  Optional arguments
    -p [ --pool ] arg     pool name
    --image arg           image name
    --snap arg            snapshot name
    -o [ --options ] arg  map options
    --read-only           map read-only
    --exclusive           disable automatic exclusive lock transitions
  
  rbd help merge-diff
  usage: rbd merge-diff [--path <path>] [--no-progress] 
                        <diff1-path> <diff2-path> <path-name> 
  
  Merge two diff exports together.
  
  Positional arguments
    <diff1-path>         path to first diff (or '-' for stdin)
    <diff2-path>         path to second diff
    <path-name>          path to merged diff (or '-' for stdout)
  
  Optional arguments
    --path arg           path to merged diff (or '-' for stdout)
    --no-progress        disable progress output
  
  rbd help mirror image demote
  usage: rbd mirror image demote [--pool <pool>] [--image <image>] 
                                 <image-spec> 
  
  Demote an image to non-primary for RBD mirroring.
  
  Positional arguments
    <image-spec>         image specification
                         (example: [<pool-name>/]<image-name>)
  
  Optional arguments
    -p [ --pool ] arg    pool name
    --image arg          image name
  
  rbd help mirror image disable
  usage: rbd mirror image disable [--force] [--pool <pool>] [--image <image>] 
                                  <image-spec> 
  
  Disable RBD mirroring for an image.
  
  Positional arguments
    <image-spec>         image specification
                         (example: [<pool-name>/]<image-name>)
  
  Optional arguments
    --force              disable even if not primary
    -p [ --pool ] arg    pool name
    --image arg          image name
  
  rbd help mirror image enable
  usage: rbd mirror image enable [--pool <pool>] [--image <image>] 
                                 <image-spec> 
  
  Enable RBD mirroring for an image.
  
  Positional arguments
    <image-spec>         image specification
                         (example: [<pool-name>/]<image-name>)
  
  Optional arguments
    -p [ --pool ] arg    pool name
    --image arg          image name
  
  rbd help mirror image promote
  usage: rbd mirror image promote [--force] [--pool <pool>] [--image <image>] 
                                  <image-spec> 
  
  Promote an image to primary for RBD mirroring.
  
  Positional arguments
    <image-spec>         image specification
                         (example: [<pool-name>/]<image-name>)
  
  Optional arguments
    --force              promote even if not cleanly demoted by remote cluster
    -p [ --pool ] arg    pool name
    --image arg          image name
  
  rbd help mirror image resync
  usage: rbd mirror image resync [--pool <pool>] [--image <image>] 
                                 <image-spec> 
  
  Force resync to primary image for RBD mirroring.
  
  Positional arguments
    <image-spec>         image specification
                         (example: [<pool-name>/]<image-name>)
  
  Optional arguments
    -p [ --pool ] arg    pool name
    --image arg          image name
  
  rbd help mirror image status
  usage: rbd mirror image status [--pool <pool>] [--image <image>] 
                                 [--format <format>] [--pretty-format] 
                                 <image-spec> 
  
  Show RDB mirroring status for an image.
  
  Positional arguments
    <image-spec>         image specification
                         (example: [<pool-name>/]<image-name>)
  
  Optional arguments
    -p [ --pool ] arg    pool name
    --image arg          image name
    --format arg         output format (plain, json, or xml) [default: plain]
    --pretty-format      pretty formatting (json and xml)
  
  rbd help mirror pool demote
  usage: rbd mirror pool demote [--pool <pool>] 
                                <pool-name> 
  
  Demote all primary images in the pool.
  
  Positional arguments
    <pool-name>          pool name
  
  Optional arguments
    -p [ --pool ] arg    pool name
  
  rbd help mirror pool disable
  usage: rbd mirror pool disable [--pool <pool>] 
                                 <pool-name> 
  
  Disable RBD mirroring by default within a pool.
  
  Positional arguments
    <pool-name>          pool name
  
  Optional arguments
    -p [ --pool ] arg    pool name
  
  rbd help mirror pool enable
  usage: rbd mirror pool enable [--pool <pool>] 
                                <pool-name> <mode> 
  
  Enable RBD mirroring by default within a pool.
  
  Positional arguments
    <pool-name>          pool name
    <mode>               mirror mode [image or pool]
  
  Optional arguments
    -p [ --pool ] arg    pool name
  
  rbd help mirror pool info
  usage: rbd mirror pool info [--pool <pool>] [--format <format>] 
                              [--pretty-format] 
                              <pool-name> 
  
  Show information about the pool mirroring configuration.
  
  Positional arguments
    <pool-name>          pool name
  
  Optional arguments
    -p [ --pool ] arg    pool name
    --format arg         output format (plain, json, or xml) [default: plain]
    --pretty-format      pretty formatting (json and xml)
  
  rbd help mirror pool peer add
  usage: rbd mirror pool peer add [--pool <pool>] 
                                  [--remote-client-name <remote-client-name>] 
                                  [--remote-cluster <remote-cluster>] 
                                  <pool-name> <remote-cluster-spec> 
  
  Add a mirroring peer to a pool.
  
  Positional arguments
    <pool-name>              pool name
    <remote-cluster-spec>    remote cluster spec
                             (example: [<client name>@]<cluster name>
  
  Optional arguments
    -p [ --pool ] arg        pool name
    --remote-client-name arg remote client name
    --remote-cluster arg     remote cluster name
  
  rbd help mirror pool peer remove
  usage: rbd mirror pool peer remove [--pool <pool>] 
                                     <pool-name> <uuid> 
  
  Remove a mirroring peer from a pool.
  
  Positional arguments
    <pool-name>          pool name
    <uuid>               peer uuid
  
  Optional arguments
    -p [ --pool ] arg    pool name
  
  rbd help mirror pool peer set
  usage: rbd mirror pool peer set [--pool <pool>] 
                                  <pool-name> <uuid> <key> <value> 
  
  Update mirroring peer settings.
  
  Positional arguments
    <pool-name>          pool name
    <uuid>               peer uuid
    <key>                peer parameter [client or cluster]
    <value>              new client or cluster name
  
  Optional arguments
    -p [ --pool ] arg    pool name
  
  rbd help mirror pool promote
  usage: rbd mirror pool promote [--force] [--pool <pool>] 
                                 <pool-name> 
  
  Promote all non-primary images in the pool.
  
  Positional arguments
    <pool-name>          pool name
  
  Optional arguments
    --force              promote even if not cleanly demoted by remote cluster
    -p [ --pool ] arg    pool name
  
  rbd help mirror pool status
  usage: rbd mirror pool status [--pool <pool>] [--format <format>] 
                                [--pretty-format] [--verbose] 
                                <pool-name> 
  
  Show status for all mirrored images in the pool.
  
  Positional arguments
    <pool-name>          pool name
  
  Optional arguments
    -p [ --pool ] arg    pool name
    --format arg         output format (plain, json, or xml) [default: plain]
    --pretty-format      pretty formatting (json and xml)
    --verbose            be verbose
  
  rbd help nbd list
  usage: rbd nbd list 
  
  List the nbd devices already used.
  
  rbd help nbd map
  usage: rbd nbd map [--pool <pool>] [--image <image>] [--snap <snap>] 
                     [--read-only] [--exclusive] [--device <device>] 
                     [--nbds_max <nbds_max>] [--max_part <max_part>] 
                     <image-or-snap-spec> 
  
  Map image to a nbd device.
  
  Positional arguments
    <image-or-snap-spec>  image or snapshot specification
                          (example: [<pool-name>/]<image-name>[@<snap-name>])
  
  Optional arguments
    -p [ --pool ] arg     pool name
    --image arg           image name
    --snap arg            snapshot name
    --read-only           map read-only
    --exclusive           forbid writes by other clients
    --device arg          specify nbd device
    --nbds_max arg        override module param nbds_max
    --max_part arg        override module param max_part
  
  rbd help nbd unmap
  usage: rbd nbd unmap 
                       <device-spec> 
  
  Unmap a nbd device.
  
  Positional arguments
    <device-spec>        specify nbd device
  
  rbd help object-map check
  usage: rbd object-map check [--pool <pool>] [--image <image>] [--snap <snap>] 
                              [--no-progress] 
                              <image-or-snap-spec> 
  
  Verify the object map is correct.
  
  Positional arguments
    <image-or-snap-spec>  image or snapshot specification
                          (example: [<pool-name>/]<image-name>[@<snap-name>])
  
  Optional arguments
    -p [ --pool ] arg     pool name
    --image arg           image name
    --snap arg            snapshot name
    --no-progress         disable progress output
  
  rbd help object-map rebuild
  usage: rbd object-map rebuild [--pool <pool>] [--image <image>] 
                                [--snap <snap>] [--no-progress] 
                                <image-or-snap-spec> 
  
  Rebuild an invalid object map.
  
  Positional arguments
    <image-or-snap-spec>  image or snapshot specification
                          (example: [<pool-name>/]<image-name>[@<snap-name>])
  
  Optional arguments
    -p [ --pool ] arg     pool name
    --image arg           image name
    --snap arg            snapshot name
    --no-progress         disable progress output
  
  rbd help pool init
  usage: rbd pool init [--pool <pool>] [--force] 
                       <pool-name> 
  
  Initialize pool for use by RBD.
  
  Positional arguments
    <pool-name>          pool name
  
  Optional arguments
    -p [ --pool ] arg    pool name
    --force              force initialize pool for RBD use if registered by
                         another application
  
  rbd help remove
  usage: rbd remove [--pool <pool>] [--image <image>] [--no-progress] 
                    <image-spec> 
  
  Delete an image.
  
  Positional arguments
    <image-spec>         image specification
                         (example: [<pool-name>/]<image-name>)
  
  Optional arguments
    -p [ --pool ] arg    pool name
    --image arg          image name
    --no-progress        disable progress output
  
  rbd help rename
  usage: rbd rename [--pool <pool>] [--image <image>] [--dest-pool <dest-pool>] 
                    [--dest <dest>] 
                    <source-image-spec> <dest-image-spec> 
  
  Rename image within pool.
  
  Positional arguments
    <source-image-spec>  source image specification
                         (example: [<pool-name>/]<image-name>)
    <dest-image-spec>    destination image specification
                         (example: [<pool-name>/]<image-name>)
  
  Optional arguments
    -p [ --pool ] arg    source pool name
    --image arg          source image name
    --dest-pool arg      destination pool name
    --dest arg           destination image name
  
  rbd help resize
  usage: rbd resize [--pool <pool>] [--image <image>] --size <size> 
                    [--allow-shrink] [--no-progress] 
                    <image-spec> 
  
  Resize (expand or shrink) image.
  
  Positional arguments
    <image-spec>         image specification
                         (example: [<pool-name>/]<image-name>)
  
  Optional arguments
    -p [ --pool ] arg    pool name
    --image arg          image name
    -s [ --size ] arg    image size (in M/G/T) [default: M]
    --allow-shrink       permit shrinking
    --no-progress        disable progress output
  
  rbd help showmapped
  usage: rbd showmapped [--format <format>] [--pretty-format] 
  
  Show the rbd images mapped by the kernel.
  
  Optional arguments
    --format arg         output format (plain, json, or xml) [default: plain]
    --pretty-format      pretty formatting (json and xml)
  
  rbd help snap create
  usage: rbd snap create [--pool <pool>] [--image <image>] [--snap <snap>] 
                         <snap-spec> 
  
  Create a snapshot.
  
  Positional arguments
    <snap-spec>          snapshot specification
                         (example: [<pool-name>/]<image-name>@<snapshot-name>)
  
  Optional arguments
    -p [ --pool ] arg    pool name
    --image arg          image name
    --snap arg           snapshot name
  
  rbd help snap limit clear
  usage: rbd snap limit clear [--pool <pool>] [--image <image>] 
                              <image-spec> 
  
  Remove snapshot limit.
  
  Positional arguments
    <image-spec>         image specification
                         (example: [<pool-name>/]<image-name>)
  
  Optional arguments
    -p [ --pool ] arg    pool name
    --image arg          image name
  
  rbd help snap limit set
  usage: rbd snap limit set [--pool <pool>] [--image <image>] [--limit <limit>] 
                            <image-spec> 
  
  Limit the number of snapshots.
  
  Positional arguments
    <image-spec>         image specification
                         (example: [<pool-name>/]<image-name>)
  
  Optional arguments
    -p [ --pool ] arg    pool name
    --image arg          image name
    --limit arg          maximum allowed snapshot count
  
  rbd help snap list
  usage: rbd snap list [--pool <pool>] [--image <image>] [--image-id <image-id>] 
                       [--format <format>] [--pretty-format] 
                       <image-spec> 
  
  Dump list of image snapshots.
  
  Positional arguments
    <image-spec>         image specification
                         (example: [<pool-name>/]<image-name>)
  
  Optional arguments
    -p [ --pool ] arg    pool name
    --image arg          image name
    --image-id arg       image id
    --format arg         output format (plain, json, or xml) [default: plain]
    --pretty-format      pretty formatting (json and xml)
  
  rbd help snap protect
  usage: rbd snap protect [--pool <pool>] [--image <image>] [--snap <snap>] 
                          <snap-spec> 
  
  Prevent a snapshot from being deleted.
  
  Positional arguments
    <snap-spec>          snapshot specification
                         (example: [<pool-name>/]<image-name>@<snapshot-name>)
  
  Optional arguments
    -p [ --pool ] arg    pool name
    --image arg          image name
    --snap arg           snapshot name
  
  rbd help snap purge
  usage: rbd snap purge [--pool <pool>] [--image <image>] 
                        [--image-id <image-id>] [--no-progress] 
                        <image-spec> 
  
  Delete all snapshots.
  
  Positional arguments
    <image-spec>         image specification
                         (example: [<pool-name>/]<image-name>)
  
  Optional arguments
    -p [ --pool ] arg    pool name
    --image arg          image name
    --image-id arg       image id
    --no-progress        disable progress output
  
  rbd help snap remove
  usage: rbd snap remove [--pool <pool>] [--image <image>] [--snap <snap>] 
                         [--no-progress] [--image-id <image-id>] [--force] 
                         <snap-spec> 
  
  Delete a snapshot.
  
  Positional arguments
    <snap-spec>          snapshot specification
                         (example: [<pool-name>/]<image-name>@<snapshot-name>)
  
  Optional arguments
    -p [ --pool ] arg    pool name
    --image arg          image name
    --snap arg           snapshot name
    --no-progress        disable progress output
    --image-id arg       image id
    --force              flatten children and unprotect snapshot if needed.
  
  rbd help snap rename
  usage: rbd snap rename [--pool <pool>] [--image <image>] [--snap <snap>] 
                         [--dest-pool <dest-pool>] [--dest <dest>] 
                         [--dest-snap <dest-snap>] 
                         <source-snap-spec> <dest-snap-spec> 
  
  Rename a snapshot.
  
  Positional arguments
    <source-snap-spec>   source snapshot specification
                         (example: [<pool-name>/]<image-name>@<snapshot-name>)
    <dest-snap-spec>     destination snapshot specification
                         (example: [<pool-name>/]<image-name>@<snapshot-name>)
  
  Optional arguments
    -p [ --pool ] arg    source pool name
    --image arg          source image name
    --snap arg           source snapshot name
    --dest-pool arg      destination pool name
    --dest arg           destination image name
    --dest-snap arg      destination snapshot name
  
  rbd help snap rollback
  usage: rbd snap rollback [--pool <pool>] [--image <image>] [--snap <snap>] 
                           [--no-progress] 
                           <snap-spec> 
  
  Rollback image to snapshot.
  
  Positional arguments
    <snap-spec>          snapshot specification
                         (example: [<pool-name>/]<image-name>@<snapshot-name>)
  
  Optional arguments
    -p [ --pool ] arg    pool name
    --image arg          image name
    --snap arg           snapshot name
    --no-progress        disable progress output
  
  rbd help snap unprotect
  usage: rbd snap unprotect [--pool <pool>] [--image <image>] [--snap <snap>] 
                            [--image-id <image-id>] 
                            <snap-spec> 
  
  Allow a snapshot to be deleted.
  
  Positional arguments
    <snap-spec>          snapshot specification
                         (example: [<pool-name>/]<image-name>@<snapshot-name>)
  
  Optional arguments
    -p [ --pool ] arg    pool name
    --image arg          image name
    --snap arg           snapshot name
    --image-id arg       image id
  
  rbd help status
  usage: rbd status [--pool <pool>] [--image <image>] [--format <format>] 
                    [--pretty-format] 
                    <image-spec> 
  
  Show the status of this image.
  
  Positional arguments
    <image-spec>         image specification
                         (example: [<pool-name>/]<image-name>)
  
  Optional arguments
    -p [ --pool ] arg    pool name
    --image arg          image name
    --format arg         output format (plain, json, or xml) [default: plain]
    --pretty-format      pretty formatting (json and xml)
  
  rbd help trash list
  usage: rbd trash list [--pool <pool>] [--all] [--long] [--format <format>] 
                        [--pretty-format] 
                        <pool-name> 
  
  List trash images.
  
  Positional arguments
    <pool-name>          pool name
  
  Optional arguments
    -p [ --pool ] arg    pool name
    -a [ --all ]         list images from all sources
    -l [ --long ]        long listing format
    --format arg         output format (plain, json, or xml) [default: plain]
    --pretty-format      pretty formatting (json and xml)
  
  rbd help trash move
  usage: rbd trash move [--pool <pool>] [--image <image>] [--delay <delay>] 
                        <image-spec> 
  
  Move an image to the trash.
  
  Positional arguments
    <image-spec>         image specification
                         (example: [<pool-name>/]<image-name>)
  
  Optional arguments
    -p [ --pool ] arg    pool name
    --image arg          image name
    --delay arg          time delay in seconds until effectively remove the image
  
  rbd help trash remove
  usage: rbd trash remove [--pool <pool>] [--image-id <image-id>] 
                          [--no-progress] [--force] 
                          <image-id> 
  
  Remove an image from trash.
  
  Positional arguments
    <image-id>           image id
                         (example: [<pool-name>/]<image-id>)
  
  Optional arguments
    -p [ --pool ] arg    pool name
    --image-id arg       image id
    --no-progress        disable progress output
    --force              force remove of non-expired delayed images
  
  rbd help trash restore
  usage: rbd trash restore [--pool <pool>] [--image-id <image-id>] 
                           [--image <image>] 
                           <image-id> 
  
  Restore an image from trash.
  
  Positional arguments
    <image-id>           image id
                         (example: [<pool-name>/]<image-id>)
  
  Optional arguments
    -p [ --pool ] arg    pool name
    --image-id arg       image id
    --image arg          image name
  
  rbd help unmap
  usage: rbd unmap [--pool <pool>] [--image <image>] [--snap <snap>] 
                   [--options <options>] 
                   <image-or-snap-or-device-spec> 
  
  Unmap a rbd device that was used by the kernel.
  
  Positional arguments
    <image-or-snap-or-device-spec>  image, snapshot, or device specification
                                    [<pool-name>/]<image-name>[@<snapshot-name>]
                                    or <device-path>
  
  Optional arguments
    -p [ --pool ] arg               pool name
    --image arg                     image name
    --snap arg                      snapshot name
    -o [ --options ] arg            unmap options
  
  rbd help watch
  usage: rbd watch [--pool <pool>] [--image <image>] 
                   <image-spec> 
  
  Watch events on image.
  
  Positional arguments
    <image-spec>         image specification
                         (example: [<pool-name>/]<image-name>)
  
  Optional arguments
    -p [ --pool ] arg    pool name
    --image arg          image name
  
