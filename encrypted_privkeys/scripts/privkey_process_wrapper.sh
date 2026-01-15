#!/bin/bash

script_dir=$(dirname $(readlink -f $0))

# We just need to ignore the first parameter that will be the certificate being decoded
$script_dir/privkey_process.sh "${@:2}" || { echo "Error decoding $1" >&2; exit 1; }
