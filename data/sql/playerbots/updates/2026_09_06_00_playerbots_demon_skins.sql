-- #########################################################
-- Playerbots - Dark Chaos bot cosmetics: warlock minion skins
--
-- Warlock minions are five fixed creature entries, so every warlock
-- bot on the server summons an identical imp. DCBotCosmetics.cpp
-- re-skins them from this pool. Visual only: the pet keeps its own
-- entry, family, stats and abilities.
--
-- Seeded from the CreatureDisplayInfo -> CreatureModelData master
-- CSVs by demon model directory, then gated twice: the display must
-- have a creature_model_info row, and must already be worn by a
-- creature in creature_template_model - which proves the M2 resolves
-- in the deployed client MPQ chain.
-- #########################################################

CREATE TABLE IF NOT EXISTS `playerbots_demon_skins` (
  `family` INT(11) NOT NULL COMMENT 'CreatureFamily of the minion this skin belongs to',
  `display_id` INT(11) NOT NULL COMMENT 'CreatureDisplayInfo id applied to the pet',
  `kind` VARCHAR(32) NOT NULL DEFAULT '' COMMENT 'Readable family name',
  `enabled` TINYINT(3) NOT NULL DEFAULT 1,
  PRIMARY KEY (`family`, `display_id`),
  KEY `enabled` (`enabled`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8;

-- doomguard (CreatureFamily 19) - 34 skins
DELETE FROM `playerbots_demon_skins` WHERE `family` = 19;
INSERT INTO `playerbots_demon_skins` (`family`, `display_id`, `kind`, `enabled`) VALUES
(19, 68, 'doomguard', 1),
(19, 433, 'doomguard', 1),
(19, 1912, 'doomguard', 1),
(19, 2727, 'doomguard', 1),
(19, 4426, 'doomguard', 1),
(19, 4427, 'doomguard', 1),
(19, 16269, 'doomguard', 1),
(19, 16982, 'doomguard', 1),
(19, 17095, 'doomguard', 1),
(19, 17096, 'doomguard', 1),
(19, 17097, 'doomguard', 1),
(19, 17098, 'doomguard', 1),
(19, 17099, 'doomguard', 1),
(19, 17100, 'doomguard', 1),
(19, 18166, 'doomguard', 1),
(19, 18169, 'doomguard', 1),
(19, 18237, 'doomguard', 1),
(19, 18311, 'doomguard', 1),
(19, 18373, 'doomguard', 1),
(19, 18753, 'doomguard', 1),
(19, 18821, 'doomguard', 1),
(19, 19200, 'doomguard', 1),
(19, 19594, 'doomguard', 1),
(19, 19719, 'doomguard', 1),
(19, 19945, 'doomguard', 1),
(19, 20606, 'doomguard', 1),
(19, 20645, 'doomguard', 1),
(19, 20919, 'doomguard', 1),
(19, 20930, 'doomguard', 1),
(19, 20974, 'doomguard', 1),
(19, 21430, 'doomguard', 1),
(19, 22809, 'doomguard', 1),
(19, 500594, 'doomguard', 1),
(19, 503755, 'doomguard', 1);

-- succubus (CreatureFamily 17) - 20 skins
DELETE FROM `playerbots_demon_skins` WHERE `family` = 17;
INSERT INTO `playerbots_demon_skins` (`family`, `display_id`, `kind`, `enabled`) VALUES
(17, 159, 'succubus', 1),
(17, 2737, 'succubus', 1),
(17, 2834, 'succubus', 1),
(17, 4162, 'succubus', 1),
(17, 10070, 'succubus', 1),
(17, 10191, 'succubus', 1),
(17, 10923, 'succubus', 1),
(17, 10924, 'succubus', 1),
(17, 10925, 'succubus', 1),
(17, 10926, 'succubus', 1),
(17, 10927, 'succubus', 1),
(17, 19199, 'succubus', 1),
(17, 19887, 'succubus', 1),
(17, 19947, 'succubus', 1),
(17, 19948, 'succubus', 1),
(17, 20214, 'succubus', 1),
(17, 20385, 'succubus', 1),
(17, 20387, 'succubus', 1),
(17, 21456, 'succubus', 1),
(17, 22671, 'succubus', 1);

-- felhunter (CreatureFamily 15) - 13 skins
DELETE FROM `playerbots_demon_skins` WHERE `family` = 15;
INSERT INTO `playerbots_demon_skins` (`family`, `display_id`, `kind`, `enabled`) VALUES
(15, 850, 'felhunter', 1),
(15, 1913, 'felhunter', 1),
(15, 6172, 'felhunter', 1),
(15, 6173, 'felhunter', 1),
(15, 6688, 'felhunter', 1),
(15, 7949, 'felhunter', 1),
(15, 7972, 'felhunter', 1),
(15, 10950, 'felhunter', 1),
(15, 16313, 'felhunter', 1),
(15, 17321, 'felhunter', 1),
(15, 502771, 'felhunter', 1),
(15, 502772, 'felhunter', 1),
(15, 502773, 'felhunter', 1);

-- voidwalker (CreatureFamily 16) - 38 skins
DELETE FROM `playerbots_demon_skins` WHERE `family` = 16;
INSERT INTO `playerbots_demon_skins` (`family`, `display_id`, `kind`, `enabled`) VALUES
(16, 1130, 'voidwalker', 1),
(16, 1131, 'voidwalker', 1),
(16, 1132, 'voidwalker', 1),
(16, 14428, 'voidwalker', 1),
(16, 16575, 'voidwalker', 1),
(16, 16634, 'voidwalker', 1),
(16, 17081, 'voidwalker', 1),
(16, 17298, 'voidwalker', 1),
(16, 17704, 'voidwalker', 1),
(16, 17746, 'voidwalker', 1),
(16, 18050, 'voidwalker', 1),
(16, 18069, 'voidwalker', 1),
(16, 18170, 'voidwalker', 1),
(16, 18366, 'voidwalker', 1),
(16, 18919, 'voidwalker', 1),
(16, 18953, 'voidwalker', 1),
(16, 18955, 'voidwalker', 1),
(16, 18957, 'voidwalker', 1),
(16, 18958, 'voidwalker', 1),
(16, 18988, 'voidwalker', 1),
(16, 18990, 'voidwalker', 1),
(16, 18998, 'voidwalker', 1),
(16, 19196, 'voidwalker', 1),
(16, 19338, 'voidwalker', 1),
(16, 19681, 'voidwalker', 1),
(16, 19882, 'voidwalker', 1),
(16, 19951, 'voidwalker', 1),
(16, 19952, 'voidwalker', 1),
(16, 23372, 'voidwalker', 1),
(16, 26208, 'voidwalker', 1),
(16, 26214, 'voidwalker', 1),
(16, 27855, 'voidwalker', 1),
(16, 36688, 'voidwalker', 1),
(16, 502735, 'voidwalker', 1),
(16, 502736, 'voidwalker', 1),
(16, 502737, 'voidwalker', 1),
(16, 503318, 'voidwalker', 1),
(16, 504306, 'voidwalker', 1);

-- imp (CreatureFamily 23) - 28 skins
DELETE FROM `playerbots_demon_skins` WHERE `family` = 23;
INSERT INTO `playerbots_demon_skins` (`family`, `display_id`, `kind`, `enabled`) VALUES
(23, 4449, 'imp', 1),
(23, 7552, 'imp', 1),
(23, 10811, 'imp', 1),
(23, 10812, 'imp', 1),
(23, 10815, 'imp', 1),
(23, 10817, 'imp', 1),
(23, 12190, 'imp', 1),
(23, 14380, 'imp', 1),
(23, 16480, 'imp', 1),
(23, 16888, 'imp', 1),
(23, 16889, 'imp', 1),
(23, 16890, 'imp', 1),
(23, 16891, 'imp', 1),
(23, 17035, 'imp', 1),
(23, 17610, 'imp', 1),
(23, 18038, 'imp', 1),
(23, 18510, 'imp', 1),
(23, 18878, 'imp', 1),
(23, 19344, 'imp', 1),
(23, 19611, 'imp', 1),
(23, 19618, 'imp', 1),
(23, 19718, 'imp', 1),
(23, 19892, 'imp', 1),
(23, 19944, 'imp', 1),
(23, 20570, 'imp', 1),
(23, 21040, 'imp', 1),
(23, 21048, 'imp', 1),
(23, 23269, 'imp', 1);

-- felguard (CreatureFamily 29) - 40 skins
DELETE FROM `playerbots_demon_skins` WHERE `family` = 29;
INSERT INTO `playerbots_demon_skins` (`family`, `display_id`, `kind`, `enabled`) VALUES
(29, 5047, 'felguard', 1),
(29, 5048, 'felguard', 1),
(29, 5049, 'felguard', 1),
(29, 7970, 'felguard', 1),
(29, 9015, 'felguard', 1),
(29, 9016, 'felguard', 1),
(29, 9017, 'felguard', 1),
(29, 9018, 'felguard', 1),
(29, 9129, 'felguard', 1),
(29, 14152, 'felguard', 1),
(29, 14255, 'felguard', 1),
(29, 15298, 'felguard', 1),
(29, 15301, 'felguard', 1),
(29, 17542, 'felguard', 1),
(29, 18193, 'felguard', 1),
(29, 18251, 'felguard', 1),
(29, 18287, 'felguard', 1),
(29, 18342, 'felguard', 1),
(29, 18345, 'felguard', 1),
(29, 18420, 'felguard', 1),
(29, 18448, 'felguard', 1),
(29, 18525, 'felguard', 1),
(29, 18531, 'felguard', 1),
(29, 18654, 'felguard', 1),
(29, 18832, 'felguard', 1),
(29, 19597, 'felguard', 1),
(29, 19722, 'felguard', 1),
(29, 19977, 'felguard', 1),
(29, 19982, 'felguard', 1),
(29, 20040, 'felguard', 1),
(29, 20041, 'felguard', 1),
(29, 20044, 'felguard', 1),
(29, 20045, 'felguard', 1),
(29, 20046, 'felguard', 1),
(29, 20047, 'felguard', 1),
(29, 20531, 'felguard', 1),
(29, 21307, 'felguard', 1),
(29, 21365, 'felguard', 1),
(29, 22811, 'felguard', 1),
(29, 22812, 'felguard', 1);
