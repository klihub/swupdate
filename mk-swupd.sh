#!/bin/sh

usage () {
    local _ec=$1

    shift

    if [ -n "$*" ]; then
        echo "$*"
    fi

    cat <<EOF
usage: $0 --version <update-version> --url <content-url> [var0=val0, ...]

  -p,--product    product name (will be used for the output file name)
  -v,--version    generate update for the given version
  -u,--url        fetch updates from the given content URL
  -h,--help       print this help message
EOF

    exit $_ec
}

generate_swu () {
    local _out="$1" _version="$2" _url="$3" _variables _i _v _var _val _t
    shift 3
    _variables="$*"

    > sw-description cat <<EOF
software =
{
    version = "0.0.1";

    images = ( {
        type = "swupd";
        filename = "swupd.cfg";
    } )
}
EOF

    > swupd.cfg cat <<EOF
swupd = {
    content_url = "$_url";
    version = $_version;
EOF

    for _v in $_variables; do
        echo "Processing variable $_v..."
        _var=${_v%=*}
        _val=${_v#*=}
        echo "name: $_var, value: $_val"
        case "$_val" in
                   \"*) echo "    $_var = $_val;" >> swupd.cfg;;
            true|false) echo "    $_var = $_val;" >> swupd.cfg;;
                     *)
                        echo "... $_var = $_val ..."
                        _t="${_val//[0-9]}"
                        if [ -z "$_t" -o "$_t" = "." ]; then
                            echo "    $_var = $_val;" >> swupd.cfg
                        else
                            echo "    $_var = \"$_val\";" >> swupd.cfg
                        fi
                        ;;
        esac
    done
    echo "}" >> swupd.cfg

    for _i in sw-description swupd.cfg; do
        echo $_i
    done | cpio -ov -H crc > $_out && rm -f sw-description swupd.cfg
}

#########################
# main script

set -e

PRODUCT=""
VERSION=""
URL=""
SWU=""
VARIABLES=""

while [ -n "$1" ]; do
    case "$1" in
        -p|--product) PRODUCT="$2"; shift 2;;
        -v|--version) VERSION="$2"; shift 2;;
        -u|--url)     URL="$2"; shift 2;;
        -h|--help)    usage 0 "" ;;
        *=*)          VARIABLES="$VARIABLES $1"; shift 1;;
        *)            usage 1 "unknown option $1"
    esac
done

if [ -z "$PRODUCT" ]; then
    usage 1 "missing product name (-p)"
fi

if [ -z "$VERSION" ]; then
    usage 1 "missing version name (-v)"
fi

if [ -z "$URL" ]; then
    usage 1 "missing content URL (-u)"
fi

case $URL in
    */);;
    *) URL="$URL/";;
esac

SWU=${PRODUCT}_${VERSION}.swu
echo "Generating $SWU (with $VARIABLES)..."

if ! generate_swu $SWU $VERSION $URL $VARIABLES; then
    rm -f $SWU
else
    echo "Done."
fi

