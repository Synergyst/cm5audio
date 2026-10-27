#!/bin/bash
# /root/Sources/cm5audio/scripts/update-asound-conf.sh

ASOUND_CONF="/etc/asound.conf"
STATE_FILE="/var/lib/cm5audio/cards.sig"
mkdir -p /var/lib/cm5audio

CURRENT_SIG=$(cat /proc/asound/cards)

if [ -f "$STATE_FILE" ] && [ "$(cat "$STATE_FILE")" = "$CURRENT_SIG" ]; then
    exit 0
fi

echo "ALSA card layout change detected. Regenerating /etc/asound.conf..."

CARDS_COMMENT="# ========== Cards (by stable IDs) ==========\n"
while IFS= read -r line; do
    CARDS_COMMENT+="# $line\n"
done < <(awk '
{
    if (index($0, "[") > 0 && index($0, "]") > 0) {
        card_num = $1;
        sub(/[^0-9].*/, "", card_num);
        
        start = index($0, "[");
        end = index($0, "]");
        id = substr($0, start+1, end-start-1);
        sub(/[[:space:]]+$/, "", id);
        
        colon = index($0, ":");
        desc = substr($0, colon+1);
        sub(/^[[:space:]]+/, "", desc);
        sub(/[[:space:]]+$/, "", desc);
        
        printf "card %s ID: %-15s (%s)\n", card_num, id, desc
    }
}' /proc/asound/cards)

cat << CONFIG > "$ASOUND_CONF"
$(echo -e "$CARDS_COMMENT")

# ========== USB PnP Sound Device (Card 0): capture softvol ==========
pcm.pnp_raw {
    type hw
    card Device
    device 0
}

pcm.pnp_softvol {
    type softvol
    slave.pcm "pnp_raw"
    control {
        name "USB PnP Softvol"
        card Device
    }
    min_dB -60.0
    max_dB 12.0
}

pcm.pnp {
    type plug
    slave.pcm "pnp_softvol"
}

# ========== HiFiBerry DAC8x: 8-ch playback softvol ==========
pcm.dac8x_raw {
    type hw
    card sndrpihifiberry
    device 0
}

pcm.dac8x_softvol {
    type softvol
    slave.pcm "dac8x_raw"
    control {
        name "DAC8x Softvol"
        card sndrpihifiberry
    }
    min_dB -60.0
    max_dB 12.0
}

pcm.dac8x {
    type plug
    slave.pcm "dac8x_softvol"
}

# ========== UAC2 Gadget (Card 4): stereo playback softvol ==========
pcm.uac2_raw {
    type hw
    card UAC2Gadget
    device 0
}

pcm.uac2_softvol {
    type softvol
    slave.pcm "uac2_raw"
    control {
        name "UAC2 Softvol"
        card UAC2Gadget
    }
    channels 2
    min_dB -60.0
    max_dB 12.0
}

pcm.uac2 {
    type plug
    slave.pcm "uac2_softvol"
}

# ========== UAC2 Gadget 1 (Card 5): stereo playback softvol ==========
pcm.uac2_1_raw {
    type hw
    card UAC2Gadget_1
    device 0
}

pcm.uac2_1_softvol {
    type softvol
    slave.pcm "uac2_1_raw"
    control {
        name "UAC2_1 Softvol"
        card UAC2Gadget_1
    }
    channels 2
    min_dB -60.0
    max_dB 12.0
}

pcm.uac2_1 {
    type plug
    slave.pcm "uac2_1_softvol"
}

# ========== Default control interface ==========
ctl.!default {
    type hw
    card sndrpihifiberry
}

# ========== Optional: set a default playback PCM ==========
# Uncomment below if you want system "default" audio to go to the HiFiBerry 8ch softvol:
# pcm.!default {
#   type asym
#   playback.pcm "dac8x"
#   capture.pcm "hw:AT2020USB,0"
# }
CONFIG

echo "$CURRENT_SIG" > "$STATE_FILE"
echo "/etc/asound.conf successfully updated for current card topology."
