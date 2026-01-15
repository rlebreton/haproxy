#!/bin/sh

set -e

SCRIPT_DIR=$(dirname $(readlink -f $0))
GPG_SECRET_FILE=$SCRIPT_DIR/secure_secret.gpg
FILENAME=
ENCRYPT=
GET_PASS=

usage () {
    cat << EOF
    Usage: $0 [--encrypt|--get-pass|--help] [<filename>]

    Parameters:
        --encrypt, Encrypt the SSL private key provided on stdin thanks to a
            machine specific passphrase. Dump the encrypted passphrase on stdout

        --get-pass, Return the machine specific passphrase used to encrypt passphrases
EOF
    exit 1
}

# Check that a specific command is available
check_command () {
    [ -z $1 ] && return 1

    command -v "$1" > /dev/null || { echo "'$1' not found" >&2; return 1; }
}

check_ext_tools() {
    check_command hostname
    check_command ip
    check_command sha256sum
    check_command openssl
    check_command gpg
}

# Build a machine specific UUID out of the hostname and the first interface's MAC address
gen_machine_id() {
    HOSTNAME=$(hostname)

    # Get first active physical network interface (otehr than loopback)
    #FIRST_ITF=$(ip -o link show | awk -F': ' '$2 !~ /lo/ {print $2; exit}')
    FIRST_ITF=$(ip -o link show | grep -v " lo:" | head -n1 | sed -r 's/^[^:]*: ([^:]*):.*/\1/')

    # Extract the MAC address of the selected interface
    # We read directly from sysfs to avoid parsing localized command output
    MAC_ADDR=$(cat /sys/class/net/"$FIRST_ITF"/address 2>/dev/null)

    if [ -z "$MAC_ADDR" ]; then
        echo "Error: Network interface or MAC address not found." >&2
        return 1
    fi

    # Generate the Unique Machine ID
    # We concatenate Hostname and MAC, then hash them using SHA-256
    # This creates a fixed-length, anonymized unique fingerprint
    MACHINE_ID=$(echo -n "${HOSTNAME}${MAC_ADDR}" | sha256sum | cut -d' ' -f1)

    echo "$MACHINE_ID"
}

# Build a unique 16B hex passphrase and store it in a gpg file protected by the
# machine-specific UUID
gen_gpg_secret () {
    local UUID
    local VAL

    [ -f $GPG_SECRET_FILE ] && return 0

    UUID=$(gen_machine_id)

    # Generate 16 bytes random number
    VAL=$(openssl rand -hex 16)

    # Encrypt the value using GPG
    echo -n "$VAL" | gpg --batch --yes --pinentry-mode loopback --passphrase "$UUID" -c -o $GPG_SECRET_FILE
}

# Get the machine specific passphrase out of the already existing secret gpg file
# protected by the machine UUID
get_secret () {
    local UUID
    local DECRYPTED_VAL

    UUID=$(gen_machine_id)

    DECRYPTED_VAL=$(gpg --batch --quiet --yes --pinentry-mode loopback --passphrase "$UUID" --decrypt $GPG_SECRET_FILE)

    echo -n $DECRYPTED_VAL
}

# Encrypt the private key provided on stdin by using the passphrase stored in the
# local gpg secret file.
# Dump the encrypted private key on stdout.
do_encrypt () {
    # Protected machine specific passphrase
    if [ ! -f $GPG_SECRET_FILE ]
    then
        gen_gpg_secret
    fi

    # Encrypt private key provided on stdin and dump it on stdout
    openssl pkey -aes256 -passout pass:"$(get_secret)" -in /dev/stdin -out /dev/stdout # 2>/dev/null
}

# Dump the passphrase used to encrypt private keys on stdout
do_get_pass () {

    [ ! -f $GPG_SECRET_FILE ] && { echo "Missing GPG secret file" >&2; return 1; }

    echo -n "$(get_secret)"
}


while [ -n "$1" ]
do
    case "$1" in
        --encrypt)  ENCRYPT=1 ; shift ;;
        --get-pass) GET_PASS=1 ; shift ;;
        --help)     usage ; shift ;;
        *)          FILENAME=$2 ; shift ;;
    esac
done

check_ext_tools

if [ ! -z $ENCRYPT ]
then
    do_encrypt
elif [ ! -z $GET_PASS ]
then
    do_get_pass
else
    usage
fi
