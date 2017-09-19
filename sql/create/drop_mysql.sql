REVOKE ALL PRIVILEGES ON * . * FROM 'mopcore'@'localhost';

REVOKE ALL PRIVILEGES ON `world` . * FROM 'mopcore'@'localhost';

REVOKE GRANT OPTION ON `world` . * FROM 'mopcore'@'localhost';

REVOKE ALL PRIVILEGES ON `characters` . * FROM 'mopcore'@'localhost';

REVOKE GRANT OPTION ON `characters` . * FROM 'mopcore'@'localhost';

REVOKE ALL PRIVILEGES ON `auth` . * FROM 'mopcore'@'localhost';

REVOKE GRANT OPTION ON `auth` . * FROM 'mopcore'@'localhost';

DROP USER 'mopcore'@'localhost';

DROP DATABASE IF EXISTS `world`;

DROP DATABASE IF EXISTS `characters`;

DROP DATABASE IF EXISTS `auth`;