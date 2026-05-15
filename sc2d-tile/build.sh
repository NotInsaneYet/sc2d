#!/bin/bash
set -e

SDK=$HOME/android-sdk
AAPT2=$SDK/build-tools/34.0.0/aapt2
D8=$SDK/build-tools/34.0.0/d8
ZIPALIGN=$SDK/build-tools/34.0.0/zipalign
APKSIGNER=$SDK/build-tools/34.0.0/apksigner
PLATFORM=$SDK/platforms/android-28
PROJ=$HOME/sc2d-tile
OUT=$PROJ/build

rm -rf $OUT
mkdir -p $OUT/dex $OUT/obj $OUT/apk

# 1. Compile resources
echo "[1/5] Compiling resources..."
$AAPT2 compile -o $OUT/obj/res.zip \
  $PROJ/res/drawable/ic_tile.xml \
  $PROJ/res/values/strings.xml

# 2. Link resources → APK
echo "[2/5] Linking resources..."
$AAPT2 link -o $OUT/apk/unaligned.apk \
  -I $PLATFORM/android.jar \
  --manifest $PROJ/AndroidManifest.xml \
  $OUT/obj/res.zip

# 3. Compile Java → classes
echo "[3/5] Compiling Java..."
/home/alx/jdk21/bin/javac --release 11 -cp $PLATFORM/android.jar \
  -d $OUT/obj \
  $PROJ/src/sc2d/tile/QsTile.java 2>&1

# 4. Convert .class → DEX
echo "[4/5] Converting to DEX..."
$D8 \
  --lib $PLATFORM/android.jar \
  --min-api 28 \
  --output $OUT/dex \
  $OUT/obj/sc2d/tile/*.class

# Add classes.dex to APK
cd $OUT/dex && zip -q $OUT/apk/unaligned.apk classes.dex && cd $PROJ

# 5. Sign & align
echo "[5/5] Signing..."
if [ ! -f $PROJ/keystore.jks ]; then
  keytool -genkey -v -keystore $PROJ/keystore.jks \
    -alias sc2d -keyalg RSA -keysize 2048 -validity 10000 \
    -storepass password -keypass password \
    -dname "CN=SC2D, O=sc2d, C=US"
fi

$ZIPALIGN -f -p 4 $OUT/apk/unaligned.apk $OUT/apk/unsigned.apk
$APKSIGNER sign \
  --ks $PROJ/keystore.jks \
  --ks-pass pass:password \
  --key-pass pass:password \
  --out $OUT/sc2d-tile.apk \
  $OUT/apk/unsigned.apk

rm -f $OUT/dex/classes.dex $OUT/apk/unaligned.apk $OUT/apk/unsigned.apk
echo ""
echo "=== DONE: $OUT/sc2d-tile.apk ==="
