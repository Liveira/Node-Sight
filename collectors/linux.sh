#!/bin/sh
# Read-only collector. Runs in the SSH session; nothing is installed on the host.
export LC_ALL=C
# Restrict sampling intervals to values supported by the desktop app.
interval=${1:-2}
case "$interval" in 0.1|0.25|0.5|1|2|5|10) ;; *) interval=2 ;; esac
# Prefer the default route to avoid counting bridge traffic twice.
iface=$(awk '$2 == "00000000" { print $1; exit }' /proc/net/route)
thermal=""
# Prefer a CPU package sensor; any readable thermal zone is only a fallback.
for zone in /sys/class/thermal/thermal_zone*; do
    [ -r "$zone/temp" ] || continue
    [ -n "$thermal" ] || thermal="$zone/temp"
    if grep -Eq 'x86_pkg_temp|cpu-thermal|cpu_thermal' "$zone/type" 2>/dev/null; then
        thermal="$zone/temp"
        break
    fi
done
disk="0 0"
tick=0
while :; do
    # Cache df for about thirty seconds, independently of the requested sample interval.
    if [ "$tick" -eq 0 ]; then
        disk=$(df -Pk / 2>/dev/null | awk 'NR==2 {print $2, $3}')
        tick=$(awk -v seconds="$interval" 'BEGIN { n=int(30/seconds); print n<1?1:n }')
    fi
    # Explicit file inputs prevent consuming the SSH script stream on stdin.
    awk -v disk="$disk" -v iface="$iface" -v thermal="$thermal" '
        # Ignore guest fields already included in user/nice; count iowait as idle.
        FILENAME == "/proc/stat" && $1 == "cpu" {
            for (i=2; i<=9 && i<=NF; i++) total += $i;
            idle=$5+$6
        }
        # MemAvailable includes reclaimable cache, unlike MemFree.
        FILENAME == "/proc/meminfo" {
            if ($1=="MemTotal:") mt=$2;
            if ($1=="MemAvailable:") ma=$2;
            if ($1=="SwapTotal:") st=$2;
            if ($1=="SwapFree:") sf=$2
        }
        # RX/TX are cumulative bytes; the client calculates rates.
        FILENAME == "/proc/net/dev" && index($0, ":") {
            split($0, p, ":"); gsub(/ /,"",p[1]);
            if ((iface!="" && p[1]==iface) || (iface=="" && p[1]!="lo")) {
                gsub(/^ +/,"",p[2]); split(p[2], n, / +/); rx+=n[1]; tx+=n[9]
            }
        }
        FILENAME == "/proc/uptime" {up=$1}
        FILENAME == "/proc/loadavg" {load=$1}
        # Emit one RV1 frame; a negative temperature means no readable sensor.
        END {
            temp=-1;
            if (thermal!="" && (getline t < thermal)>0) temp=t/1000;
            split(disk,d," ");
            printf "RV1 %.0f %.0f %.0f %.0f %.0f %.0f %.0f %.0f %.0f %.0f %.2f %.2f %.1f\n", total,idle,mt,ma,st,sf,d[1],d[2],rx,tx,up,load,temp
        }
    ' /proc/stat /proc/meminfo /proc/net/dev /proc/uptime /proc/loadavg || exit
    tick=$((tick - 1))
    # Stop after a broken session instead of leaving a detached collector loop.
    sleep "$interval" || exit
done
