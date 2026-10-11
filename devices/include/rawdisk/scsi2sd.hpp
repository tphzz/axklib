// SCSI2SD support. See scsi2sd/v6.hpp (configuration on the card) and scsi2sd/xml.hpp
// (scsi2sd-util exports, required for V5). Both produce a rawdisk::VolumeSet; open a disk with
// rawdisk::open_volume(card, set, "<scsi id>", access).
#pragma once

#include "rawdisk/scsi2sd/common.hpp"
#include "rawdisk/scsi2sd/v6.hpp"
#include "rawdisk/scsi2sd/xml.hpp"
#include "rawdisk/volume.hpp"
