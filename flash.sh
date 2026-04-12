#!/usr/bin/env bash

set -euo pipefail

# ─── Paths ──────────────────────────────────────────────────────────────────
script_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
project_dir="${script_dir}"
config_dir="${XDG_CONFIG_HOME:-$HOME/.config}/smd-flash"
config_file="${config_dir}/config"

# ─── Colors ──────────────────────────────────────────────────────────────────
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
CYAN='\033[0;36m'
MAGENTA='\033[0;35m'
BOLD='\033[1m'
NC='\033[0m'

# ─── Board Presets ───────────────────────────────────────────────────────────
# Format:  "Display Name|fqbn|build_suffix"
declare -a BOARD_PRESETS=(
  "Arduino Nano (ATmega328P Old Bootloader)|arduino:avr:nano:cpu=atmega328old|nano-old"
  "Arduino Nano (ATmega328P)|arduino:avr:nano:cpu=atmega328|nano"
  "Arduino Uno|arduino:avr:uno|uno"
  "ESP32 Dev Module|esp32:esp32:esp32|esp32"
  "ESP32-S3 Dev Module|esp32:esp32:esp32s3|esp32s3"
  "ESP32-C3 Dev Module|esp32:esp32:esp32c3|esp32c3"
)

# ─── Defaults (overridden by config) ─────────────────────────────────────────
fqbn="arduino:avr:nano:cpu=atmega328old"
saved_port=""
build_suffix="nano-old"

# ─── Runtime flags ───────────────────────────────────────────────────────────
port="${ARDUINO_PORT:-}"
compile_only=0
upload_only=0
select_board_flag=0
set_board_only=0

# ─── Config helpers ──────────────────────────────────────────────────────────
load_config() {
  if [[ -f "${config_file}" ]]; then
    # shellcheck source=/dev/null
    source "${config_file}"
  fi
}

save_config() {
  mkdir -p "${config_dir}"
  cat >"${config_file}" <<EOF
# SMD Flash configuration — auto-generated $(date '+%Y-%m-%d %H:%M')
fqbn="${fqbn}"
build_suffix="${build_suffix}"
saved_port="${saved_port}"
EOF
}

build_dir_for_suffix() {
  echo "${BUILD_DIR:-/tmp/$(basename "${project_dir}")-${build_suffix}-build}"
}

# ─── Helper: interactive arrow-key menu ──────────────────────────────────────
# Usage: arrow_menu "Title" item1 item2 ...
# Sets $ARROW_MENU_RESULT to the index chosen, $ARROW_MENU_VALUE to the value.
arrow_menu() {
  local title="$1"; shift
  local -a items=("$@")
  local count=${#items[@]}
  local selected=0
  local key key_esc

  echo -e "${BLUE}==>${NC} ${CYAN}${title}${NC} (↑↓ arrows, Enter to select, q to quit):"

  # Hide cursor
  printf "\e[?25l"
  trap 'printf "\e[?25h"' EXIT INT TERM

  while true; do
    for i in "${!items[@]}"; do
      if [[ "$i" -eq "$selected" ]]; then
        printf "  ${GREEN}▶ \e[7m %s \e[27m${NC}\e[K\n" "${items[$i]}"
      else
        printf "    %s \e[K\n" "${items[$i]}"
      fi
    done

    read -rsn1 key
    if [[ -z "${key}" ]]; then
      break
    elif [[ "${key}" == "q" || "${key}" == "Q" ]]; then
      printf "\e[?25h"
      trap - EXIT INT TERM
      echo -e "${YELLOW}Aborted.${NC}"
      exit 0
    elif [[ "${key}" == $'\e' ]]; then
      read -rsn2 -t 0.1 key_esc || true
      if [[ "${key_esc}" == "[A" ]]; then
        ((selected--)) || true
        if [[ ${selected} -lt 0 ]]; then selected=$((count - 1)); fi
      elif [[ "${key_esc}" == "[B" ]]; then
        ((selected++)) || true
        if [[ ${selected} -ge ${count} ]]; then selected=0; fi
      fi
    fi

    printf "\e[%dA" "${count}"
  done

  printf "\e[?25h"
  trap - EXIT INT TERM

  ARROW_MENU_RESULT="${selected}"
  ARROW_MENU_VALUE="${items[$selected]}"
}

# ─── Board selector ──────────────────────────────────────────────────────────
select_board() {
  local -a display_names=()
  for preset in "${BOARD_PRESETS[@]}"; do
    display_names+=("${preset%%|*}")
  done

  arrow_menu "Select target board" "${display_names[@]}"

  local chosen_preset="${BOARD_PRESETS[$ARROW_MENU_RESULT]}"
  local name fqbn_new suffix
  IFS='|' read -r name fqbn_new suffix <<<"${chosen_preset}"

  fqbn="${fqbn_new}"
  build_suffix="${suffix}"

  echo -e "${GREEN}✓ Board set:${NC} ${CYAN}${name}${NC}"
  echo -e "  FQBN : ${MAGENTA}${fqbn}${NC}"
  save_config
}

# ─── Port menu ───────────────────────────────────────────────────────────────
show_port_menu() {
  local -a ports=()
  local -a descriptions=()
  local line candidate

  while IFS= read -r line; do
    if [[ -z "${line}" || "${line}" == Port* ]]; then continue; fi
    candidate="${line%%[[:space:]]*}"
    if [[ "${candidate}" == /dev/* || "${candidate}" == COM* ]]; then
      ports+=("${candidate}")
      descriptions+=("${line}")
    fi
  done < <(arduino-cli board list)

  local count=${#ports[@]}

  if [[ "${count}" -eq 0 ]]; then
    echo -e "${RED}✗ No serial ports detected.${NC}" >&2
    echo -e "${YELLOW}Connect a board and try again, or use: $(basename "$0") -p /dev/ttyUSB0${NC}" >&2
    return 1
  fi

  # If saved port is still present, auto-select it
  if [[ -n "${saved_port}" ]]; then
    for i in "${!ports[@]}"; do
      if [[ "${ports[$i]}" == "${saved_port}" ]]; then
        port="${saved_port}"
        echo -e "${GREEN}✓ Using remembered port:${NC} ${CYAN}${port}${NC}"
        return 0
      fi
    done
    echo -e "${YELLOW}⚠ Remembered port ${saved_port} not found — please select again.${NC}"
  fi

  if [[ "${count}" -eq 1 ]]; then
    port="${ports[0]}"
    echo -e "${GREEN}✓ Auto-detected port:${NC} ${CYAN}${port}${NC}"
    saved_port="${port}"
    save_config
    return 0
  fi

  arrow_menu "Select serial port" "${descriptions[@]}"

  port="${ports[$ARROW_MENU_RESULT]}"
  saved_port="${port}"
  save_config
  echo -e "${GREEN}✓ Selected port:${NC} ${CYAN}${port}${NC}"
  return 0
}

# ─── Compile & Upload ─────────────────────────────────────────────────────────
run_compile() {
  local build_dir
  build_dir="$(build_dir_for_suffix)"
  mkdir -p "${build_dir}"
  echo -e "${BLUE}==>${NC} ${GREEN}Compiling${NC} ${BOLD}$(basename "${project_dir}")${NC} for ${CYAN}${fqbn}${NC}..."
  if arduino-cli compile \
    --clean \
    --quiet \
    --build-path "${build_dir}" \
    --fqbn "${fqbn}" \
    "${project_dir}"; then
    echo -e "${GREEN}✓ Compilation finished.${NC}"
  else
    echo -e "${RED}✗ Compilation failed!${NC}" >&2
    exit 1
  fi
}

run_upload() {
  local build_dir
  build_dir="$(build_dir_for_suffix)"
  local upload_port="${port}"

  if [[ -z "${upload_port}" ]]; then
    if ! show_port_menu; then exit 1; fi
    upload_port="${port}"
  else
    # Still update saved_port when port given via -p flag
    saved_port="${upload_port}"
    save_config
  fi

  echo -e "${BLUE}==>${NC} ${YELLOW}Uploading${NC} to ${CYAN}${upload_port}${NC} with ${MAGENTA}${fqbn}${NC}..."
  if arduino-cli upload \
    --input-dir "${build_dir}" \
    -p "${upload_port}" \
    --fqbn "${fqbn}" \
    "${project_dir}" >/dev/null; then
    echo -e "${GREEN}✓ Upload successful!${NC}"
  else
    echo -e "${RED}✗ Upload failed!${NC}" >&2
    exit 1
  fi
}

# ─── Usage ───────────────────────────────────────────────────────────────────
usage() {
  cat <<EOF
${BOLD}SMD Flash Script${NC}
Flash Arduino/ESP32 firmware with persistent board & port memory.

${BOLD}Usage:${NC}
  $(basename "$0") [OPTIONS]

${BOLD}Options:${NC}
  -p, --port PORT       Serial port, e.g. /dev/ttyUSB0
  -b, --board           Open board selector menu (and save choice)
      --set-board       Select board and exit (no compile/upload)
      --compile-only    Compile only, skip upload
      --upload-only     Skip compile, upload only
  -h, --help            Show this help

${BOLD}Environment:${NC}
  ARDUINO_PORT          Override port (env var)
  BUILD_DIR             Override build artifact directory

${BOLD}Config file:${NC}  ${config_file}
${BOLD}Current board:${NC} ${fqbn}
${BOLD}Remembered port:${NC} ${saved_port:-<none>}
EOF
}

# ─── Main ────────────────────────────────────────────────────────────────────
load_config  # Load saved fqbn, build_suffix, saved_port

while [[ $# -gt 0 ]]; do
  case "$1" in
    -p|--port)
      [[ $# -lt 2 ]] && { echo "Missing value for $1" >&2; usage >&2; exit 1; }
      port="$2"; shift 2 ;;
    -b|--board)
      select_board_flag=1; shift ;;
    --set-board)
      set_board_only=1; shift ;;
    --compile-only)
      compile_only=1; shift ;;
    --upload-only)
      upload_only=1; shift ;;
    -h|--help)
      usage; exit 0 ;;
    *)
      echo "Unknown argument: $1" >&2; usage >&2; exit 1 ;;
  esac
done

# Board selection (interactive or forced)
if [[ "${set_board_only}" -eq 1 || "${select_board_flag}" -eq 1 ]]; then
  select_board
  [[ "${set_board_only}" -eq 1 ]] && exit 0
fi

if [[ "${compile_only}" -eq 1 && "${upload_only}" -eq 1 ]]; then
  echo "Choose either --compile-only or --upload-only, not both." >&2
  exit 1
fi

# Show current board info at start
echo -e "${BLUE}Board:${NC} ${CYAN}${fqbn}${NC}  ${YELLOW}(use -b to change)${NC}"

if [[ "${upload_only}" -eq 0 ]]; then run_compile; fi
if [[ "${compile_only}" -eq 0 ]]; then run_upload; fi
