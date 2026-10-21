#!/bin/bash
# usage: ./pack.sh <version>  -> builds, zips dist/TurnBasedKH2-<version>.zip and prints size + sha256
set -e
V=$1
sed -i "s/#define TBKH2_VERSION \".*\"/#define TBKH2_VERSION \"$V\"/" src/common.h
powershell.exe -NoProfile -ExecutionPolicy Bypass -File build.ps1 -Version $V 2>&1 | grep -E "error|built"
d=dist/TurnBasedKH2-$V
cp package/README.txt package/LICENSE.txt package/THIRD_PARTY_LICENSES.txt package/steam_appid.txt $d/
python -c "
import zipfile,os
d='$d'
with zipfile.ZipFile(d+'.zip','w',zipfile.ZIP_DEFLATED) as z:
    for f in ['dinput8.dll','steam_appid.txt','README.txt','LICENSE.txt','THIRD_PARTY_LICENSES.txt']: z.write(os.path.join(d,f),f)
"
stat -c %s $d.zip; sha256sum $d.zip | cut -d' ' -f1
