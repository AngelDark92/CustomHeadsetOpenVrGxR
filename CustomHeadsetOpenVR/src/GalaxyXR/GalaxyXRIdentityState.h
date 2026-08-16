#pragma once

#include "../Config/Config.h"

namespace galaxyxr {

inline bool RestoreConnectedHeadsetBackup(
	Config::HeadsetType& connectedHeadset,
	bool& backupValid,
	Config::HeadsetType previousConnectedHeadset){
	if(!backupValid){
		return false;
	}
	connectedHeadset = previousConnectedHeadset;
	backupValid = false;
	return true;
}

} // namespace galaxyxr
