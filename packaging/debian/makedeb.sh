#!/bin/sh
set -eu

root_path=$1
package_path=$(mktemp -d)
trap 'rm -rf "$package_path"' EXIT HUP INT TERM
chmod 755 "$package_path"

make -C "$root_path" install DESTDIR="$package_path" PREFIX=
mkdir -p "$package_path/DEBIAN"

atop_version=$(sed -n 's/^#define[[:space:]]*ATOPVERS[[:space:]]*"\([^"]*\)".*/\1/p' "$root_path/atop/version.h")
test -n "$atop_version"
architecture=$(dpkg --print-architecture)
package_size=$(du -sk "$package_path" | cut -f1)
runtime_depends="atop (>= $atop_version), atop (<< 2.14), libc6, zlib1g"
if readelf -d "$root_path/atophttpd" | grep -q 'libssl'; then
    runtime_depends="$runtime_depends, libssl3 | libssl3t64"
fi

sed -e "s/ATOP_VERSION/$atop_version/g" \
    -e "s/ATOPHTTPD_SIZE/$package_size/g" \
    -e "s/ATOP_ARCHITECTURE/$architecture/g" \
    -e "s/RUNTIME_DEPENDS/$runtime_depends/g" \
    "$root_path/packaging/debian/control" > "$package_path/DEBIAN/control"

dpkg-deb --root-owner-group --build "$package_path" "$root_path/atophttpd_${atop_version}_${architecture}.deb"
