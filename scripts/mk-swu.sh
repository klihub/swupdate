#!/bin/sh

usage () {
    local _ec=$1

    shift

    if [ -n "$*" ]; then
        echo "$*"
    fi

    cat <<EOF
usage: $0 -p <product> -t <type> -v <version> -u <url> [var0=val0, ...] [-c <collection>]

  -t,--type <type>        update backend type (swupd, casync, ostree, etc.)
  -p,--product <product>  product name (will be used for the output file name)
  -v,--version <version>  generate update for the given version
  -u,--url <url>          fetch updates from the given URL
  -c,--collection <c>     generate the specified software collection layout
  -k,--keep               keep generated intermediate files
  -h,--help               print this help message

  A collection <c> is specified as <swset> <mode1>:<device1>[:<volume1>]...
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


generate_description () {
    local _version="$1" _i _var _mode _dev _vol

    if [ $NSET -gt 0 ]; then
        > sw-description cat <<EOF
software =
{
    version = "0.0.1";

    $SWSET = {
EOF

        _i=0
        while [ $_i -lt $NSET ]; do
            _var=MODE$_i; _mode=${!_var}
            _var=DEV$_i; _dev=${!_var}
            _var=VOL$_i; _vol=${!_var}

            >> sw-description cat <<EOF
        $_mode: {
            images = ( {
                type = "$TYPE";
                filename = "$TYPE.cfg";
                device = "$_dev";
EOF

            if [ -n "$_vol" ]; then
                echo "                volume = \"$_vol\";" >> sw-description
            fi

            echo "            } );" >> sw-description
            echo "        }" >> sw-description
            _i=$(expr $_i + 1)
        done

        echo "    };" >> sw-description
        echo "}" >> sw-description
    else
        > sw-description <<EOF
software =
{
    version = "0.0.1";

    images = ( {
        type = "$TYPE";
        filename = "$TYPE.cfg";
    } )
}
EOF
    fi
}


generate_cfg () {
    local _out="$1" _version="$2" _url="$3" _variables _i _v _var _val _t
    shift 3
    _variables="$*"

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
}


generate_swu () {
    local _out="$1" _version="$2" _url="$3" _variables _i
    shift 3
    _variables="$*"

    generate_description $_version
    generate_cfg $_out $_version $_url $_variables

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
NSET=0

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
        -c|--collection)
            SWSET=$2
            shift 2
            while true; do
                case $1 in
                    *:*)
                        mode=${1%%:*}
                        dev=${1#*:}
                        vol=${dev#*:}
                        dev=${dev%:*}
                        if [ "$vol" = "$dev" ]; then
                            vol=""
                        fi
                        eval "MODE$NSET=$mode;DEV$NSET=$dev;VOL$NSET=$vol"
                        NSET=$(expr $NSET + 1)
                        shift
                        ;;
                      *)
                        break
                        ;;
                esac
            done
            ;;

        *)
            usage 1 "unknown option $1"
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
