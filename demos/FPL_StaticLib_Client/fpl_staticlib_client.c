/*
-------------------------------------------------------------------------------
Name:
	FPL-Demo | StaticLib_Client

Description:
	Client application using the FPL static library

Requirements:
	- C99 Compiler
	- Final Platform Layer
	- StaticLib_Host (Static-Library)

Author:
	Torsten Spaete

Changelog:
	## 2018-09-24
	- Reflect api changes in FPL 0.9.2

	## 2018-04-24:
	- Initial creation of this description block
-------------------------------------------------------------------------------
*/

#define FPL_ENTRYPOINT // Force the inclusion of the entry point
#include <final_platform_layer.h>

int main(int argc, char **argv) {
	if(fplPlatformInit(fplInitFlags_All, fpl_null)) {
		fplEvent ev;
		while(fplWindowUpdate(&ev)) {
			fplEvent ev;
			while(fplPollEvent(&ev)) {}
		}
		fplPlatformRelease();
	}
	return 0;
}