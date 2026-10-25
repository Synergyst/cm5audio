#!/usr/bin/env bash

parse_device_type() {
    local stream_type=$1 # "p" or "c"
    local stream_label=$2 # "Playback" or "Capture"

    echo "### ${stream_label} Devices"
    echo ""
    echo "| Device ID | Card Name | Device Name | Channels | Sample Rates | Bit Depths / Formats | Access Modes |"
    echo "|---|---|---|---|---|---|---|"

    for pcm in /proc/asound/card*/pcm*${stream_type}; do
        [[ -e "$pcm" ]] || continue

        card_num=$(echo "$pcm" | sed -E 's/.*card([0-9]+).*/\1/')
        dev_num=$(echo "$pcm" | sed -E 's/.*pcm([0-9]+).*/\1/')
        hw_id="hw:${card_num},${dev_num}"

        card_name=$(cat "/proc/asound/card${card_num}/id" 2>/dev/null \vert{}\vert{} echo "Card $card_num")
        card_name="${card_name//|/-}"

        dev_name=$(awk -F': ' '/^name:/ {print $2}' "${pcm}/info" 2>/dev/null | xargs)
        if [ -z "$dev_name" ]; then
            dev_name="-"
        fi
        dev_name="${dev_name//|/-}"

        # Query hardware parameters without forcing an incompatible format constraint
        if [ "$stream_type" = "p" ]; then
            raw_output=$(timeout 2 aplay -D "$hw_id" --dump-hw-params < /dev/zero 2>&1)
            exit_code=$?
        else
            raw_output=$(timeout 2 arecord -D "$hw_id" --dump-hw-params 2>&1)
            exit_code=$?
        fi

        if [ $exit_code -eq 124 ]; then
            formats="*TIMEOUT (Driver blocked)*"
            channels="-"
            rates="-"
            access="-"
        else
            formats=$(echo "$raw_output" | grep "FORMAT:" | sed 's/FORMAT://g' | xargs | tr ' ' ', ')
            channels=$(echo "$raw_output" | grep "CHANNELS:" | sed 's/CHANNELS://g' | xargs)
            rates=$(echo "$raw_output" | grep "RATE:" | sed 's/RATE://g' | xargs)
            access=$(echo "$raw_output" | grep "ACCESS:" | sed 's/ACCESS://g' | xargs | tr ' ' ', ')

            if [[ "$rates" =~ ^[0-9]+ ]]; then
                read -r -a rate_array <<< "$rates"
                if [ ${#rate_array[@]} -ge 2 ]; then
                    rates="${rate_array[0]}Hz -${rate_array[-1]}Hz"
                elif [ ${#rate_array[@]} -eq 1 ]; then
                    rates="${rate_array[0]}Hz"
                fi
            fi

            if [ -z "$formats" ]; then
                formats="*Busy or restricted*"
                channels="-"
                rates="-"
                access="-"
            else
                formats="${formats//|/-}"
                access="${access//|/-}"
                channels="${channels//|/-}"
                rates="${rates//|/-}"
            fi
        fi

        printf "| \`%s\` | %s | %s | %s | %s | %s | %s |\n" \
            "$hw_id" "$card_name" "$dev_name" "$channels" "$rates" "$formats" "$access"
    done
    echo ""
}

echo "## System ALSA Hardware Capabilities"
echo ""

parse_device_type "p" "Playback"
parse_device_type "c" "Capture"
