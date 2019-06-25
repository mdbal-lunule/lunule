  $ monmaptool --create --add a 10.10.10.10:1234 /tmp/test.monmap.1234
  monmaptool: monmap file /tmp/test.monmap.1234
  monmaptool: generated fsid [0-9a-f]{8}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{12} (re)
  monmaptool: writing epoch 0 to /tmp/test.monmap.1234 (1 monitors)

  $ monmaptool --feature-list --feature-list plain --feature-list parseable /tmp/test.monmap.1234
  monmaptool: monmap file /tmp/test.monmap.1234
  MONMAP FEATURES:
      persistent: [none]
      optional:   [none]
      required:   [none]
  
  AVAILABLE FEATURES:
      supported:  [kraken(1),luminous(2)]
      persistent: [kraken(1),luminous(2)]
  MONMAP FEATURES:
      persistent: [none]
      optional:   [none]
      required:   [none]
  
  AVAILABLE FEATURES:
      supported:  [kraken(1),luminous(2)]
      persistent: [kraken(1),luminous(2)]
  monmap:persistent:[none]
  monmap:optional:[none]
  monmap:required:[none]
  available:supported:[kraken(1),luminous(2)]
  available:persistent:[kraken(1),luminous(2)]

  $ monmaptool --feature-set foo /tmp/test.monmap.1234
  unknown features name 'foo' or unable to parse value: Expected option value to be integer, got 'foo'
   usage: [--print] [--create [--clobber][--fsid uuid]]
          [--generate] [--set-initial-members]
          [--add name 1.2.3.4:567] [--rm name]
          [--feature-list [plain|parseable]]
          [--feature-set <value> [--optional|--persistent]]
          [--feature-unset <value> [--optional|--persistent]] <mapfilename>
  [1]

  $ monmaptool --feature-set kraken --feature-set 16 --optional --feature-set 32 --persistent /tmp/test.monmap.1234
  monmaptool: monmap file /tmp/test.monmap.1234
  monmaptool: writing epoch 0 to /tmp/test.monmap.1234 (1 monitors)

  $ monmaptool --feature-list /tmp/test.monmap.1234
  monmaptool: monmap file /tmp/test.monmap.1234
  MONMAP FEATURES:
      persistent: [kraken(1),unknown(32)]
      optional:   [unknown(16)]
      required:   [kraken(1),unknown(16),unknown(32)]
  
  AVAILABLE FEATURES:
      supported:  [kraken(1),luminous(2)]
      persistent: [kraken(1),luminous(2)]

  $ monmaptool --feature-unset 32 --optional --feature-list /tmp/test.monmap.1234
  monmaptool: monmap file /tmp/test.monmap.1234
  MONMAP FEATURES:
      persistent: [kraken(1),unknown(32)]
      optional:   [unknown(16)]
      required:   [kraken(1),unknown(16),unknown(32)]
  
  AVAILABLE FEATURES:
      supported:  [kraken(1),luminous(2)]
      persistent: [kraken(1),luminous(2)]
  monmaptool: writing epoch 0 to /tmp/test.monmap.1234 (1 monitors)

  $ monmaptool --feature-unset 32 --persistent --feature-unset 16 --optional --feature-list /tmp/test.monmap.1234
  monmaptool: monmap file /tmp/test.monmap.1234
  MONMAP FEATURES:
      persistent: [kraken(1)]
      optional:   [none]
      required:   [kraken(1)]
  
  AVAILABLE FEATURES:
      supported:  [kraken(1),luminous(2)]
      persistent: [kraken(1),luminous(2)]
  monmaptool: writing epoch 0 to /tmp/test.monmap.1234 (1 monitors)

  $ monmaptool --feature-unset kraken --feature-list /tmp/test.monmap.1234
  monmaptool: monmap file /tmp/test.monmap.1234
  MONMAP FEATURES:
      persistent: [none]
      optional:   [none]
      required:   [none]
  
  AVAILABLE FEATURES:
      supported:  [kraken(1),luminous(2)]
      persistent: [kraken(1),luminous(2)]
  monmaptool: writing epoch 0 to /tmp/test.monmap.1234 (1 monitors)

  $ rm /tmp/test.monmap.1234
