#!/bin/sh

usage () {
    local _ec=$1

    shift

    if [ -n "$*" ]; then
        echo "$*"
    fi

    cat <<EOF
usage: $0 --version <update-version> --url <content-url> [var0=val0, ...]

  -t,--type       update backend type (swupd, casync, ostree, etc.)
  -p,--product    product name (will be used for the output file name)
  -v,--version    generate update for the given version
  -u,--url        fetch updates from the given URL
  -k,--keep       keep generated intermediate files
  -h,--help       print this help message
EOF

    if [ -d ./hooks.d ]; then
        hookdir=./hooks.d
    elif [ -d /usr/share/swupdate/hooks.d ]; then
            hookdir=/usr/share/swupdate/hooks.d
    fi

    if [ -n "$hookdir" ]; then
        backends=$(t=""; for d in $hookdir/???*; do \
                       [ -d $d ] && echo -n "$t${d##*/}"; t=", "; \
                   done)
    fi

    if [ -n "$backends" ]; then
        echo ""
        echo "The available backends are (in $hookdir): $backends."
        echo ""
    fi

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
        type = "$TYPE";
        filename = "$TYPE.cfg";
    } )
}
EOF

    > $TYPE.cfg cat <<EOF
$TYPE = {
    update_url = "$_url";
EOF

    if [ -z "${_version//[0-9]}" ]; then
        echo "    version = $_version;" >> $TYPE.cfg
    else
        echo "    version = \"$_version\";" >> $TYPE.cfg
    fi

    for _v in $_variables; do
        echo "Processing variable $_v..."
        _var=${_v%=*}
        _val=${_v#*=}
        echo "name: $_var, value: $_val"
        case "$_val" in
                   \"*) echo "    $_var = $_val;" >> $TYPE.cfg;;
            true|false) echo "    $_var = $_val;" >> $TYPE.cfg;;
                     *)
                        echo "... $_var = $_val ..."
                        _t="${_val//[0-9]}"
                        if [ -z "$_t" -o "$_t" = "." ]; then
                            echo "    $_var = $_val;" >> $TYPE.cfg
                        else
                            echo "    $_var = \"$_val\";" >> $TYPE.cfg
                        fi
                        ;;
        esac
    done
    echo "}" >> $TYPE.cfg

    for _i in sw-description $TYPE.cfg; do
        echo $_i
    done | cpio -ov -H crc > $_out && {
        [ -n "$KEEP" ] || rm -f sw-description $TYPE.cfg
    }
}

#########################
# main script

set -e

TYPE=""
PRODUCT=""
VERSION=""
URL=""
SWU=""
VARIABLES=""
KEEP=""

while [ -n "$1" ]; do
    case "$1" in
        -t|--type)    TYPE="$2"; shift 2;;
        -b|--backend) TYPE="$2"; shift 2;;
        -p|--product) PRODUCT="$2"; shift 2;;
        -v|--version) VERSION="$2"; shift 2;;
        -u|--url)     URL="$2"; shift 2;;
        -k|--keep)    KEEP=yes; shift 1;;
        -h|--help)    usage 0 "" ;;
        *=*)          VARIABLES="$VARIABLES $1"; shift 1;;
        *)            usage 1 "unknown option $1"
    esac
done

if [ -z "$TYPE" ]; then
    usage 1 "missing update backend type (-t)"
fi

if [ -z "$PRODUCT" ]; then
    usage 1 "missing product name (-p)"
fi

if [ -z "$VERSION" ]; then
    usage 1 "missing version name (-v)"
fi

if [ -z "$URL" ]; then
    usage 1 "missing content/update URL (-u)"
fi

case $URL in
    */);;
    *) URL="$URL/";;
esac

SWU=${PRODUCT}_${VERSION}.swu
echo "Generating $SWU (for backend $TYPE with $VARIABLES)..."

if ! generate_swu $SWU $VERSION $URL $VARIABLES; then
    rm -f $SWU
else
    echo "Done."
fi
