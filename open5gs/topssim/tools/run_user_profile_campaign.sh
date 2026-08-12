#!/usr/bin/env bash
set -euo pipefail

if [[ "${TRACE:-0}" == "1" ]]; then
  set -x
fi

usage() {
  cat <<'USAGE'
Usage:
  topssim/tools/run_user_profile_campaign.sh [config.env]

Runs the TOPSSIM user-profile campaign from the Mac host:
  - verifies SSH/path/binary access on HPLMN, VPLMN, and UE
  - prepares UE Mongo tunnel and SDM cache sshfs mount
  - starts/restarts HPLMN and VPLMN cores
  - runs standard and TOPSSIM repetitions from UE
  - fetches per-run logs/audit files
  - generates logs/campaign/user-profile-campaign-report.md

Create a config first:
  cp topssim/tools/user_profile_campaign.env.example topssim/tools/user_profile_campaign.env
  edit topssim/tools/user_profile_campaign.env

Important:
  SSH keys should be installed Mac -> all VMs and UE -> HPLMN/VPLMN.
  sudo credentials on HPLMN/VPLMN may be requested once per host.
USAGE
}

if [[ "${1:-}" == "-h" || "${1:-}" == "--help" ]]; then
  usage
  exit 0
fi

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
DEFAULT_LOCAL_OPEN5GS="$(cd "$SCRIPT_DIR/../.." && pwd)"
CONFIG_FILE="${1:-$SCRIPT_DIR/user_profile_campaign.env}"

if [[ -f "$CONFIG_FILE" ]]; then
  # shellcheck disable=SC1090
  source "$CONFIG_FILE"
fi

SSH_USER="${SSH_USER:-alexis}"
SSH_KEY="${SSH_KEY:-}"
HPLMN_IP="${HPLMN_IP:-172.16.85.100}"
VPLMN_IP="${VPLMN_IP:-172.16.85.110}"
UE_IP="${UE_IP:-172.16.85.132}"

LOCAL_OPEN5GS="${LOCAL_OPEN5GS:-$DEFAULT_LOCAL_OPEN5GS}"
HPLMN_OPEN5GS="${HPLMN_OPEN5GS:-/home/alexis/TOPSSIM/open5gs}"
VPLMN_OPEN5GS="${VPLMN_OPEN5GS:-/home/alexis/TOPSSIM/open5gs}"
UE_OPEN5GS="${UE_OPEN5GS:-/home/alexis/TestBed5G/open5gs}"

HPLMN_CONFIG="${HPLMN_CONFIG:-./build/configs/examples/topssim-hplmn.yaml}"
VPLMN_CONFIG="${VPLMN_CONFIG:-./build/configs/examples/topssim-vplmn.yaml}"
UE_CONFIG="${UE_CONFIG:-./build/configs/examples/topssim-ue.yaml}"

REPETITIONS="${REPETITIONS:-10}"
YEAR="${YEAR:-2026}"
LOCAL_OUTPUT_DIR="${LOCAL_OUTPUT_DIR:-logs/campaign}"

HOME_AMF_ADDR="${HOME_AMF_ADDR:-172.16.85.128}"
VISITED_AMF_ADDR="${VISITED_AMF_ADDR:-172.16.85.148}"
TOPSSIM_SDM_CF_PLMN="${TOPSSIM_SDM_CF_PLMN:-00102}"
SKIP_GTPU_PING="${SKIP_GTPU_PING:-1}"
SKIP_PDU_SESSION="${SKIP_PDU_SESSION:-0}"
UE_TEST_TIMEOUT="${UE_TEST_TIMEOUT:-180}"
ALLOW_UE_TEST_FAILURE="${ALLOW_UE_TEST_FAILURE:-1}"

PREP_HTTP_TUNNELS="${PREP_HTTP_TUNNELS:-1}"
HPLMN_NRF_TUNNEL_ADDR="${HPLMN_NRF_TUNNEL_ADDR:-127.10.0.10}"
HPLMN_UDM_TUNNEL_ADDR="${HPLMN_UDM_TUNNEL_ADDR:-127.10.0.12}"
VPLMN_NRF_TUNNEL_ADDR="${VPLMN_NRF_TUNNEL_ADDR:-127.20.0.10}"
VPLMN_UDM_TUNNEL_ADDR="${VPLMN_UDM_TUNNEL_ADDR:-127.20.0.12}"
HPLMN_NRF_REMOTE_ADDR="${HPLMN_NRF_REMOTE_ADDR:-127.0.1.10}"
HPLMN_UDM_REMOTE_ADDR="${HPLMN_UDM_REMOTE_ADDR:-127.0.1.12}"
VPLMN_NRF_REMOTE_ADDR="${VPLMN_NRF_REMOTE_ADDR:-127.0.2.10}"
VPLMN_UDM_REMOTE_ADDR="${VPLMN_UDM_REMOTE_ADDR:-127.0.2.12}"
PREP_HTTP_PORT="${PREP_HTTP_PORT:-80}"

MONGO_TUNNEL_PORT="${MONGO_TUNNEL_PORT:-27018}"
MONGO_REMOTE_HOST="${MONGO_REMOTE_HOST:-127.0.0.1}"
MONGO_REMOTE_PORT="${MONGO_REMOTE_PORT:-27017}"
VISITED_MONGO_TUNNEL_PORT="${VISITED_MONGO_TUNNEL_PORT:-27019}"
VISITED_MONGO_REMOTE_HOST="${VISITED_MONGO_REMOTE_HOST:-127.0.0.1}"
VISITED_MONGO_REMOTE_PORT="${VISITED_MONGO_REMOTE_PORT:-27017}"

RESTART_HPLMN="${RESTART_HPLMN:-1}"
RESTART_VPLMN_EACH_RUN="${RESTART_VPLMN_EACH_RUN:-1}"
RUN_STANDARD="${RUN_STANDARD:-1}"
RUN_TOPSSIM="${RUN_TOPSSIM:-1}"
CLEAN_OUTPUT="${CLEAN_OUTPUT:-1}"
STOP_CORES_AT_END="${STOP_CORES_AT_END:-1}"
VM_SUDO_PASSWORD="${VM_SUDO_PASSWORD:-}"

CORE_BOOT_WAIT="${CORE_BOOT_WAIT:-4}"
CORE_STOP_WAIT="${CORE_STOP_WAIT:-4}"
OPEN5GS_PROCESS_NAMES="${OPEN5GS_PROCESS_NAMES:-5gc open5gs-nrfd open5gs-scpd open5gs-seppd open5gs-amfd open5gs-smfd open5gs-upfd open5gs-ausfd open5gs-udmd open5gs-udrd open5gs-pcfd open5gs-nssfd open5gs-bsfd open5gs-mmed open5gs-sgwcd open5gs-sgwud open5gs-hssd open5gs-pcrfd}"
CORE_READY_PROCESS_NAMES="${CORE_READY_PROCESS_NAMES:-open5gs-nrfd open5gs-scpd open5gs-seppd open5gs-amfd open5gs-smfd open5gs-upfd open5gs-ausfd open5gs-udmd open5gs-udrd open5gs-pcfd open5gs-nssfd open5gs-bsfd}"

HPLMN_ALIAS_SCRIPT="${HPLMN_ALIAS_SCRIPT:-./setup-aliases-vm1.sh}"
VPLMN_ALIAS_SCRIPT="${VPLMN_ALIAS_SCRIPT:-./setup-aliases-vm2.sh}"

HPLMN="$SSH_USER@$HPLMN_IP"
VPLMN="$SSH_USER@$VPLMN_IP"
UE="$SSH_USER@$UE_IP"

SSH_OPTIONS=(
  -o ServerAliveInterval=15
  -o ServerAliveCountMax=8
  -o TCPKeepAlive=yes
  -o ConnectTimeout=20
)
if [[ -n "$SSH_KEY" ]]; then
  SSH_OPTIONS=(-i "$SSH_KEY" "${SSH_OPTIONS[@]}")
fi

HPLMN_PID_FILE="/tmp/topssim-hplmn-campaign.pid"
VPLMN_PID_FILE="/tmp/topssim-vplmn-campaign.pid"

log() {
  printf '[%s] %s\n' "$(date '+%H:%M:%S')" "$*"
}

die() {
  printf 'ERROR: %s\n' "$*" >&2
  exit 1
}

require_cmd() {
  command -v "$1" >/dev/null 2>&1 || die "Missing local command: $1"
}

ssh_run() {
  local host="$1"
  shift
  ssh "${SSH_OPTIONS[@]}" "$host" "$@"
}

ssh_tty() {
  local host="$1"
  shift
  ssh -tt "${SSH_OPTIONS[@]}" "$host" "$@"
}

remote_quote() {
  printf "%q" "$1"
}

remote_sudo_helpers() {
  local password="$VM_SUDO_PASSWORD"

  if [[ -n "$password" ]]; then
    cat <<EOF
sudo_cmd() { printf "%s\\n" $(remote_quote "$password") | sudo -S -p "" "\$@"; }
sudo_validate() { printf "%s\\n" $(remote_quote "$password") | sudo -S -p "" -v; }
sudo_bg() {
  _log_file="\$1"
  shift
  nohup bash -c "printf \"%s\\\\n\" \"\\\$1\" | sudo -S -p \"\" \"\\\${@:2}\"" bash $(remote_quote "$password") "\$@" > "\$_log_file" 2>&1 < /dev/null &
  echo \$!
}
EOF
  else
    cat <<'EOF'
sudo_cmd() { sudo -n "$@"; }
sudo_validate() { sudo -n -v; }
sudo_bg() {
  _log_file="$1"
  shift
  nohup sudo -n "$@" > "$_log_file" 2>&1 < /dev/null &
  echo $!
}
EOF
  fi
}

stop_core() {
  local host="$1"
  local open5gs="$2"
  local pid_file="$3"
  local config="$4"
  local label="$5"

  log "Stopping $label core on $host"
  ssh_run "$host" "bash -lc '
    set +e
    $(remote_sudo_helpers)
    if [[ -f $(remote_quote "$pid_file") ]]; then
      pid=\$(cat $(remote_quote "$pid_file"))
      if [[ -n \"\$pid\" ]]; then
        sudo_cmd kill \"\$pid\" >/dev/null 2>&1 || true
        sleep 1
        sudo_cmd kill -9 \"\$pid\" >/dev/null 2>&1 || true
      fi
      rm -f $(remote_quote "$pid_file")
    fi
    open5gs_process_names=$(remote_quote "$OPEN5GS_PROCESS_NAMES")
    for process_name in \$open5gs_process_names; do
      sudo_cmd pkill -x "\$process_name" >/dev/null 2>&1 || true
    done
    sleep $(remote_quote "$CORE_STOP_WAIT")
    for process_name in \$open5gs_process_names; do
      if pgrep -x "\$process_name" >/dev/null 2>&1; then
        sudo_cmd pkill -9 -x "\$process_name" >/dev/null 2>&1 || true
      fi
    done
    sleep 1
    exit 0
  '"
}

start_hplmn() {
  local remote_log="$HPLMN_OPEN5GS/logs/campaign/hplmn/hplmn.log"

  if [[ "$RESTART_HPLMN" == "1" ]]; then
    stop_core "$HPLMN" "$HPLMN_OPEN5GS" "$HPLMN_PID_FILE" "$HPLMN_CONFIG" "HPLMN"
  fi

  log "Starting HPLMN core"
  ssh_run "$HPLMN" "bash -lc '
    set -euo pipefail
    $(remote_sudo_helpers)
    cd $(remote_quote "$HPLMN_OPEN5GS")
    mkdir -p logs/campaign/hplmn
    if [[ -x $(remote_quote "$HPLMN_ALIAS_SCRIPT") ]]; then
      $(remote_quote "$HPLMN_ALIAS_SCRIPT") >/dev/null
    fi
    test -x ./build/tests/app/5gc
    test -f $(remote_quote "$HPLMN_CONFIG")
    sudo_validate
    sudo_bg $(remote_quote "$remote_log") env TOPSSIM_SDM_CF=0 \
      ./build/tests/app/5gc \
      -c $(remote_quote "$HPLMN_CONFIG") \
      > $(remote_quote "$HPLMN_PID_FILE")
    sleep $(remote_quote "$CORE_BOOT_WAIT")
    core_ready_process_names=$(remote_quote "$CORE_READY_PROCESS_NAMES")
    core_ready=1
    for process_name in \$core_ready_process_names; do
      if ! pgrep -x \"\$process_name\" >/dev/null 2>&1; then
        echo \"Missing HPLMN process: \$process_name\" >&2
        core_ready=0
      fi
    done
    if [[ \"\$core_ready\" != \"1\" ]]; then
      echo \"HPLMN core failed to start. Log follows:\" >&2
      tail -120 $(remote_quote "$remote_log") >&2 || true
      echo \"Open5GS processes still running:\" >&2
      ps -eo pid,comm,args | grep -E \"5gc|open5gs-\" | grep -v grep >&2 || true
      echo \"Open5GS listening ports:\" >&2
      sudo_cmd ss -ltnup 2>/dev/null | grep -E \":(80|7777|7778|38412|2152|8805)\" >&2 || true
      exit 1
    fi
    tail -20 $(remote_quote "$remote_log") || true
  '"
}

start_vplmn() {
  local mode="$1"
  local run="$2"
  local remote_log="$3"
  local sdm_cf="0"
  local extra_env=""

  if [[ "$mode" == "topssim" ]]; then
    sdm_cf="1"
    extra_env="TOPSSIM_SDM_CF_PLMN=$TOPSSIM_SDM_CF_PLMN TOPSSIM_SDM_CACHE_DIR=$VPLMN_OPEN5GS/build/topssim/sdm-cache"
  fi

  stop_core "$VPLMN" "$VPLMN_OPEN5GS" "$VPLMN_PID_FILE" "$VPLMN_CONFIG" "VPLMN"

  log "Starting VPLMN core [$mode/$run]"
  ssh_run "$VPLMN" "bash -lc '
    set -euo pipefail
    $(remote_sudo_helpers)
    cd $(remote_quote "$VPLMN_OPEN5GS")
    mkdir -p \"\$(dirname $(remote_quote "$remote_log"))\" build/topssim/sdm-cache
    if [[ $(remote_quote "$mode") == topssim ]]; then
      rm -f build/topssim/sdm-cache/audit.jsonl
    fi
    if [[ -x $(remote_quote "$VPLMN_ALIAS_SCRIPT") ]]; then
      $(remote_quote "$VPLMN_ALIAS_SCRIPT") >/dev/null
    fi
    test -x ./build/tests/app/5gc
    test -f $(remote_quote "$VPLMN_CONFIG")
    sudo_validate
    sudo_bg $(remote_quote "$remote_log") env TOPSSIM_SDM_CF=$sdm_cf $extra_env \
      ./build/tests/app/5gc \
      -c $(remote_quote "$VPLMN_CONFIG") \
      > $(remote_quote "$VPLMN_PID_FILE")
    sleep $(remote_quote "$CORE_BOOT_WAIT")
    core_ready_process_names=$(remote_quote "$CORE_READY_PROCESS_NAMES")
    core_ready=1
    for process_name in \$core_ready_process_names; do
      if ! pgrep -x \"\$process_name\" >/dev/null 2>&1; then
        echo \"Missing VPLMN process: \$process_name\" >&2
        core_ready=0
      fi
    done
    if [[ \"\$core_ready\" != \"1\" ]]; then
      echo \"VPLMN core failed to start. Log follows:\" >&2
      tail -160 $(remote_quote "$remote_log") >&2 || true
      echo \"Open5GS processes still running:\" >&2
      ps -eo pid,comm,args | grep -E \"5gc|open5gs-\" | grep -v grep >&2 || true
      echo \"Open5GS listening ports:\" >&2
      sudo_cmd ss -ltnup 2>/dev/null | grep -E \":(80|7777|7778|38412|2152|8805)\" >&2 || true
      exit 1
    fi
    tail -20 $(remote_quote "$remote_log") || true
  '"
}

check_remote() {
  local host="$1"
  local open5gs="$2"
  local config="$3"
  local label="$4"

  log "Checking $label on $host"
  ssh_run "$host" "bash -lc '
    set -euo pipefail
    cd $(remote_quote "$open5gs")
    test -x ./build/tests/app/5gc
    test -f $(remote_quote "$config")
    hostname
  '"
}

check_ue() {
  log "Checking UE on $UE"
  ssh_run "$UE" "bash -lc '
    set -euo pipefail
    cd $(remote_quote "$UE_OPEN5GS")
    test -x ./build/tests/registration/registration
    test -f $(remote_quote "$UE_CONFIG")
    mkdir -p logs build/topssim/sdm-cache
    sed -i \"s#^db_uri:.*#db_uri: mongodb://127.0.0.1:$MONGO_TUNNEL_PORT/open5gs#\" $(remote_quote "$UE_CONFIG")
    grep \"^db_uri:\" $(remote_quote "$UE_CONFIG")
  '"
}

ensure_ue_to_core_ssh() {
  local pubkey

  log "Ensuring UE can SSH to HPLMN/VPLMN for tunnels"
  pubkey="$(ssh_run "$UE" "bash -lc '
    set -euo pipefail
    mkdir -p ~/.ssh
    chmod 700 ~/.ssh
    if [[ ! -f ~/.ssh/id_ed25519.pub ]]; then
      ssh-keygen -t ed25519 -N \"\" -f ~/.ssh/id_ed25519 >/dev/null
    fi
    cat ~/.ssh/id_ed25519.pub
  '")"

  [[ -n "$pubkey" ]] || die "Could not read UE SSH public key"

  for core_host in "$HPLMN" "$VPLMN"; do
    ssh_run "$core_host" "bash -lc '
      set -euo pipefail
      mkdir -p ~/.ssh
      chmod 700 ~/.ssh
      touch ~/.ssh/authorized_keys
      chmod 600 ~/.ssh/authorized_keys
      key=$(remote_quote "$pubkey")
      grep -qxF \"\$key\" ~/.ssh/authorized_keys || printf \"%s\\n\" \"\$key\" >> ~/.ssh/authorized_keys
    '"
  done

  ssh_run "$UE" "bash -lc '
    set -euo pipefail
    ssh -o BatchMode=yes -o StrictHostKeyChecking=accept-new $(remote_quote "$HPLMN") true
    ssh -o BatchMode=yes -o StrictHostKeyChecking=accept-new $(remote_quote "$VPLMN") true
  '"
}

prepare_ue_links() {
  log "Preparing UE Mongo tunnel and SDM cache mount"
  ssh_run "$UE" "bash -lc '
    set -euo pipefail
    cd $(remote_quote "$UE_OPEN5GS")
    mkdir -p build/topssim/sdm-cache
    if ! nc -z 127.0.0.1 $MONGO_TUNNEL_PORT >/dev/null 2>&1; then
      ssh -fN -L $MONGO_TUNNEL_PORT:$MONGO_REMOTE_HOST:$MONGO_REMOTE_PORT $(remote_quote "$HPLMN")
      sleep 1
    fi
    nc -vz 127.0.0.1 $MONGO_TUNNEL_PORT
    if ! nc -z 127.0.0.1 $VISITED_MONGO_TUNNEL_PORT >/dev/null 2>&1; then
      ssh -fN -L $VISITED_MONGO_TUNNEL_PORT:$VISITED_MONGO_REMOTE_HOST:$VISITED_MONGO_REMOTE_PORT $(remote_quote "$VPLMN")
      sleep 1
    fi
    nc -vz 127.0.0.1 $VISITED_MONGO_TUNNEL_PORT
    if ! mount | grep -q \"$(remote_quote "$UE_OPEN5GS")/build/topssim/sdm-cache\"; then
      sshfs $(remote_quote "$VPLMN:$VPLMN_OPEN5GS/build/topssim/sdm-cache") \
        $(remote_quote "$UE_OPEN5GS/build/topssim/sdm-cache")
    fi
    mount | grep topssim/sdm-cache
    if [[ $(remote_quote "$PREP_HTTP_TUNNELS") == 1 ]]; then
      $(remote_sudo_helpers)
      start_http_tunnel() {
        bind_addr=\"\$1\"
        remote_host=\"\$2\"
        remote_addr=\"\$3\"
        endpoint=\"\$bind_addr:$PREP_HTTP_PORT\"
        if ! ss -ltn | grep -Fq \"\$endpoint\"; then
          ssh -o ExitOnForwardFailure=yes -fN \
            -L \"\$bind_addr:$PREP_HTTP_PORT:\$remote_addr:$PREP_HTTP_PORT\" \
            \"\$remote_host\"
        fi
      }
      start_http_tunnel $(remote_quote "$HPLMN_NRF_TUNNEL_ADDR") $(remote_quote "$HPLMN") $(remote_quote "$HPLMN_NRF_REMOTE_ADDR")
      start_http_tunnel $(remote_quote "$HPLMN_UDM_TUNNEL_ADDR") $(remote_quote "$HPLMN") $(remote_quote "$HPLMN_UDM_REMOTE_ADDR")
      start_http_tunnel $(remote_quote "$VPLMN_NRF_TUNNEL_ADDR") $(remote_quote "$VPLMN") $(remote_quote "$VPLMN_NRF_REMOTE_ADDR")
      start_http_tunnel $(remote_quote "$VPLMN_UDM_TUNNEL_ADDR") $(remote_quote "$VPLMN") $(remote_quote "$VPLMN_UDM_REMOTE_ADDR")

      hosts_tmp=\$(mktemp)
      sed \"/# TOPSSIM UE PREP TUNNELS BEGIN/,/# TOPSSIM UE PREP TUNNELS END/d\" /etc/hosts > \"\$hosts_tmp\"
      {
        printf \"%s\\n\" \"# TOPSSIM UE PREP TUNNELS BEGIN\"
        printf \"%s %s\\n\" $(remote_quote "$HPLMN_NRF_TUNNEL_ADDR") nrf.5gc.mnc001.mcc001.3gppnetwork.org
        printf \"%s %s\\n\" $(remote_quote "$HPLMN_UDM_TUNNEL_ADDR") udm.5gc.mnc001.mcc001.3gppnetwork.org
        printf \"%s %s\\n\" $(remote_quote "$VPLMN_NRF_TUNNEL_ADDR") nrf.5gc.mnc002.mcc001.3gppnetwork.org
        printf \"%s %s\\n\" $(remote_quote "$VPLMN_UDM_TUNNEL_ADDR") udm.5gc.mnc002.mcc001.3gppnetwork.org
        printf \"%s\\n\" \"# TOPSSIM UE PREP TUNNELS END\"
      } >> \"\$hosts_tmp\"
      sudo_cmd cp \"\$hosts_tmp\" /etc/hosts
      rm -f \"\$hosts_tmp\"
      getent hosts nrf.5gc.mnc002.mcc001.3gppnetwork.org udm.5gc.mnc001.mcc001.3gppnetwork.org
      ss -ltn | grep -E \"($(remote_quote "$HPLMN_NRF_TUNNEL_ADDR")|$(remote_quote "$HPLMN_UDM_TUNNEL_ADDR")|$(remote_quote "$VPLMN_NRF_TUNNEL_ADDR")|$(remote_quote "$VPLMN_UDM_TUNNEL_ADDR")):$PREP_HTTP_PORT\" || true
    fi
  '"
}

run_ue_test() {
  local mode="$1"
  local run="$2"
  local sdm_cf="0"
  local remote_log="$UE_OPEN5GS/logs/campaign/$mode/$run/ue.log"
  local cache_dir="$UE_OPEN5GS/build/topssim/sdm-cache"
  local env_prefix

  if [[ "$mode" == "topssim" ]]; then
    sdm_cf="1"
  fi

  env_prefix="TOPSSIM_HOME_AMF_ADDR=$(remote_quote "$HOME_AMF_ADDR") TOPSSIM_VISITED_AMF_ADDR=$(remote_quote "$VISITED_AMF_ADDR") TOPSSIM_SDM_CF=$sdm_cf TOPSSIM_SKIP_GTPU_PING=$(remote_quote "$SKIP_GTPU_PING") TOPSSIM_SKIP_PDU_SESSION=$(remote_quote "$SKIP_PDU_SESSION") TOPSSIM_VISITED_DB_URI=mongodb://127.0.0.1:$VISITED_MONGO_TUNNEL_PORT/open5gs"
  if [[ "$mode" == "topssim" ]]; then
    env_prefix="$env_prefix TOPSSIM_SDM_CF_PLMN=$(remote_quote "$TOPSSIM_SDM_CF_PLMN") TOPSSIM_SDM_CACHE_DIR=$(remote_quote "$cache_dir")"
  fi

  log "Running UE test [$mode/$run]"
  ssh_run "$UE" "bash -lc '
    set -euo pipefail
    cd $(remote_quote "$UE_OPEN5GS")
    mkdir -p logs/campaign/$(remote_quote "$mode")/$(remote_quote "$run") build/topssim/sdm-cache
    set +e
    timeout $(remote_quote "$UE_TEST_TIMEOUT") env $env_prefix ./build/tests/registration/registration \
      -c $(remote_quote "$UE_CONFIG") \
      manual-lbo-detach-reattach-enhanced-test \
      2>&1 | tee $(remote_quote "$remote_log")
    test_rc=\${PIPESTATUS[0]}
    set -e
    if [[ \"\$test_rc\" -eq 124 ]]; then
      echo \"UE test timed out after $(remote_quote "$UE_TEST_TIMEOUT") seconds\" >&2
      exit 124
    fi
    if [[ \"\$test_rc\" -ne 0 ]]; then
      echo \"UE test exited with rc=\$test_rc\" >&2
      if [[ $(remote_quote "$ALLOW_UE_TEST_FAILURE") != 1 ]]; then
        exit \"\$test_rc\"
      fi
    fi
  '"
}

fetch_run_logs() {
  local mode="$1"
  local run="$2"
  local local_dir="$LOCAL_OPEN5GS/$LOCAL_OUTPUT_DIR/$mode/$run"
  local vplmn_log="$VPLMN_OPEN5GS/logs/campaign/$mode/$run/vplmn.log"
  local ue_log="$UE_OPEN5GS/logs/campaign/$mode/$run/ue.log"

  mkdir -p "$local_dir"
  log "Fetching logs [$mode/$run]"
  scp "${SSH_OPTIONS[@]}" "$VPLMN:$vplmn_log" "$local_dir/vplmn.log" >/dev/null
  scp "${SSH_OPTIONS[@]}" "$UE:$ue_log" "$local_dir/ue.log" >/dev/null

  if [[ "$mode" == "topssim" ]]; then
    scp "${SSH_OPTIONS[@]}" "$VPLMN:$VPLMN_OPEN5GS/build/topssim/sdm-cache/audit.jsonl" \
      "$local_dir/audit.jsonl" >/dev/null
  fi
}

remote_file_size() {
  local host="$1"
  local file="$2"

  ssh_run "$host" "stat -c%s $(remote_quote "$file") 2>/dev/null || echo 0"
}

slice_remote_logs() {
  local mode="$1"
  local run="$2"
  local all_log="$3"
  local start_size="$4"
  local audit_start_size="${5:-0}"
  local per_run_dir="$VPLMN_OPEN5GS/logs/campaign/$mode/$run"
  local next=$((start_size + 1))
  local audit_next=$((audit_start_size + 1))

  ssh_run "$VPLMN" "bash -lc '
    set -euo pipefail
    cd $(remote_quote "$VPLMN_OPEN5GS")
    mkdir -p $(remote_quote "$per_run_dir")
    tail -c +$next $(remote_quote "$all_log") > $(remote_quote "$per_run_dir/vplmn.log")
    if [[ $(remote_quote "$mode") == topssim && -f build/topssim/sdm-cache/audit.jsonl ]]; then
      tail -c +$audit_next build/topssim/sdm-cache/audit.jsonl > $(remote_quote "$per_run_dir/audit.jsonl")
    fi
  '"
}

run_phase() {
  local mode="$1"
  local count="$REPETITIONS"
  local i run remote_log all_log start_size audit_start_size

  if [[ "$RESTART_VPLMN_EACH_RUN" == "1" ]]; then
    for i in $(seq -w 1 "$count"); do
      run="run-$i"
      remote_log="$VPLMN_OPEN5GS/logs/campaign/$mode/$run/vplmn.log"
      log "=== $mode $run/$count ==="
      start_vplmn "$mode" "$run" "$remote_log"
      run_ue_test "$mode" "$run"
      fetch_run_logs "$mode" "$run"
      stop_core "$VPLMN" "$VPLMN_OPEN5GS" "$VPLMN_PID_FILE" "$VPLMN_CONFIG" "VPLMN"
    done
  else
    all_log="$VPLMN_OPEN5GS/logs/campaign/$mode/vplmn-all.log"
    start_vplmn "$mode" "all" "$all_log"
    for i in $(seq -w 1 "$count"); do
      run="run-$i"
      log "=== $mode $run/$count ==="
      start_size="$(remote_file_size "$VPLMN" "$all_log")"
      audit_start_size="0"
      if [[ "$mode" == "topssim" ]]; then
        audit_start_size="$(remote_file_size "$VPLMN" "$VPLMN_OPEN5GS/build/topssim/sdm-cache/audit.jsonl")"
      fi
      run_ue_test "$mode" "$run"
      slice_remote_logs "$mode" "$run" "$all_log" "$start_size" "$audit_start_size"
      fetch_run_logs "$mode" "$run"
    done
    stop_core "$VPLMN" "$VPLMN_OPEN5GS" "$VPLMN_PID_FILE" "$VPLMN_CONFIG" "VPLMN"
  fi
}

generate_report() {
  local report="$LOCAL_OPEN5GS/$LOCAL_OUTPUT_DIR/user-profile-campaign-report.md"
  local html_report="$LOCAL_OPEN5GS/$LOCAL_OUTPUT_DIR/user-profile-campaign-report.html"

  log "Generating report"
  cd "$LOCAL_OPEN5GS"
  python3 topssim/tools/user_profile_campaign_report.py \
    --standard-sepp-glob "$LOCAL_OUTPUT_DIR/standard/run-*/vplmn.log" \
    --topssim-sepp-glob "$LOCAL_OUTPUT_DIR/topssim/run-*/vplmn.log" \
    --topssim-cache-audit-glob "$LOCAL_OUTPUT_DIR/topssim/run-*/audit.jsonl" \
    --year "$YEAR" \
    > "$report"

  python3 topssim/tools/user_profile_campaign_report.py \
    --standard-sepp-glob "$LOCAL_OUTPUT_DIR/standard/run-*/vplmn.log" \
    --topssim-sepp-glob "$LOCAL_OUTPUT_DIR/topssim/run-*/vplmn.log" \
    --topssim-cache-audit-glob "$LOCAL_OUTPUT_DIR/topssim/run-*/audit.jsonl" \
    --year "$YEAR" \
    --format html \
    > "$html_report"

  log "Report written: $report"
  log "HTML report written: $html_report"
  cat "$report"
}

cleanup() {
  local status=$?
  if [[ "${STOP_CORES_AT_END:-1}" == "1" ]]; then
    stop_core "$VPLMN" "$VPLMN_OPEN5GS" "$VPLMN_PID_FILE" "$VPLMN_CONFIG" "VPLMN" || true
    stop_core "$HPLMN" "$HPLMN_OPEN5GS" "$HPLMN_PID_FILE" "$HPLMN_CONFIG" "HPLMN" || true
  fi
  exit "$status"
}

trap cleanup EXIT

main() {
  require_cmd ssh
  require_cmd scp
  require_cmd python3

  cd "$LOCAL_OPEN5GS"

  if [[ "$CLEAN_OUTPUT" == "1" ]]; then
    log "Cleaning local output: $LOCAL_OUTPUT_DIR"
    rm -rf "$LOCAL_OUTPUT_DIR"
  fi
  mkdir -p "$LOCAL_OUTPUT_DIR"

  check_remote "$HPLMN" "$HPLMN_OPEN5GS" "$HPLMN_CONFIG" "HPLMN"
  check_remote "$VPLMN" "$VPLMN_OPEN5GS" "$VPLMN_CONFIG" "VPLMN"
  check_ue
  ensure_ue_to_core_ssh
  prepare_ue_links

  start_hplmn

  if [[ "$RUN_STANDARD" == "1" ]]; then
    run_phase standard
  fi

  if [[ "$RUN_TOPSSIM" == "1" ]]; then
    run_phase topssim
  fi

  generate_report
}

main "$@"
