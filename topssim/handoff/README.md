# TOPSSIM POC Setup Instructions

This package is meant to run the TOPSSIM user-profile campaign on three cloud VMs.

Expected package layout:

```text
POC/
  open5gs/                         # Code to push/build on all VMs
  README.md                        # This file
  user_profile_campaign.cloud.env.template
  run_user_profile_campaign.sh
  config/
```

Use these placeholders in the commands below:

```bash
HPLMN_IP=<HPLMN_PUBLIC_IP>
VPLMN_IP=<VPLMN_PUBLIC_IP>
UE_IP=<UE_PUBLIC_IP>
SSH_USER=root
SSH_KEY="$HOME/.ssh/topssim_poc"
```

## 1. Provision The VMs

Create three Linux VMs:

| VM | Role |
| --- | --- |
| HPLMN | Home Open5GS core and MongoDB |
| VPLMN | Visited Open5GS core, MongoDB, and SDM-CF cache |
| UE | Open5GS registration test runner |

Open these cloud firewall/security-group rules:

| Source | Destination | Protocol/port |
| --- | --- | --- |
| Controller | all VMs | TCP 22 |
| UE | HPLMN | TCP 22 |
| UE | VPLMN | TCP 22 |
| HPLMN | VPLMN | TCP 7777, TCP 7778 |
| VPLMN | HPLMN | TCP 7777, TCP 7778 |
| UE | HPLMN | SCTP 38412 |
| UE | VPLMN | SCTP 38412 |

## 2. Install Controller SSH Key

On the controller machine:

```bash
ssh-keygen -t ed25519 -f "$SSH_KEY" -N ""

ssh-copy-id -i "$SSH_KEY.pub" "$SSH_USER@$HPLMN_IP"
ssh-copy-id -i "$SSH_KEY.pub" "$SSH_USER@$VPLMN_IP"
ssh-copy-id -i "$SSH_KEY.pub" "$SSH_USER@$UE_IP"
```

If `ssh-copy-id` is unavailable:

```bash
for host in "$HPLMN_IP" "$VPLMN_IP" "$UE_IP"; do
  cat "$SSH_KEY.pub" | ssh "$SSH_USER@$host" \
    'mkdir -p ~/.ssh && chmod 700 ~/.ssh && cat >> ~/.ssh/authorized_keys && chmod 600 ~/.ssh/authorized_keys'
done
```

Verify:

```bash
ssh -i "$SSH_KEY" "$SSH_USER@$HPLMN_IP" hostname
ssh -i "$SSH_KEY" "$SSH_USER@$VPLMN_IP" hostname
ssh -i "$SSH_KEY" "$SSH_USER@$UE_IP" hostname
```

## 3. Install OS Dependencies

Run on all three VMs:

```bash
sudo apt update
sudo apt install -y \
  git curl ca-certificates python3 python3-pip python3-setuptools python3-wheel \
  meson ninja-build build-essential pkg-config cmake bison flex \
  libsctp-dev lksctp-tools libgnutls28-dev libgcrypt20-dev libidn11-dev \
  libyaml-dev libmicrohttpd-dev libnghttp2-dev libcurl4-gnutls-dev \
  libtalloc-dev libmongoc-dev libbson-dev netcat-openbsd iproute2 rsync zip
```

Run on the UE VM only:

```bash
sudo apt install -y sshfs
```

Run on HPLMN and VPLMN only:

```bash
sudo systemctl enable --now mongod || sudo systemctl enable --now mongodb
mongosh --eval 'db.runCommand({ ping: 1 })' mongodb://127.0.0.1/open5gs
```

If MongoDB is not installed by your base image, install the distribution MongoDB package or MongoDB official package first.

## 4. Configure `/etc/hosts`

For single-public-IP cloud mode, HPLMN and VPLMN keep their internal SBI services on loopback addresses.

On HPLMN, add this block to `/etc/hosts`:

```text
# TOPSSIM HPLMN HOSTS BEGIN
127.0.1.10 nrf.5gc.mnc001.mcc001.3gppnetwork.org
127.0.1.11 ausf.5gc.mnc001.mcc001.3gppnetwork.org
127.0.1.12 udm.5gc.mnc001.mcc001.3gppnetwork.org
127.0.1.14 nssf.5gc.mnc001.mcc001.3gppnetwork.org
127.0.1.4 smf.5gc.mnc001.mcc001.3gppnetwork.org
127.0.1.15 bsf.5gc.mnc001.mcc001.3gppnetwork.org
# TOPSSIM HPLMN HOSTS END
```

On VPLMN, add this block to `/etc/hosts`:

```text
# TOPSSIM VPLMN HOSTS BEGIN
127.0.2.10 nrf.5gc.mnc002.mcc001.3gppnetwork.org
127.0.2.11 ausf.5gc.mnc002.mcc001.3gppnetwork.org
127.0.2.12 udm.5gc.mnc002.mcc001.3gppnetwork.org
127.0.2.14 nssf.5gc.mnc002.mcc001.3gppnetwork.org
127.0.2.4 smf.5gc.mnc002.mcc001.3gppnetwork.org
127.0.2.15 bsf.5gc.mnc002.mcc001.3gppnetwork.org
# TOPSSIM VPLMN HOSTS END
```

The UE `/etc/hosts` entries for NRF/UDM are normally patched automatically by the campaign script using SSH local tunnels. Do not add static UE NRF/UDM entries unless you disable `PREP_HTTP_TUNNELS`.

If cloud-init manages `/etc/hosts`, make the same changes in the cloud-init hosts template or disable `manage_etc_hosts`; otherwise changes may disappear after reboot.

## 5. Push Code To The VMs

From the controller, with the package extracted:

```bash
cd /path/to/POC

ssh -i "$SSH_KEY" "$SSH_USER@$HPLMN_IP" 'mkdir -p /home/alexis/TOPSSIM'
ssh -i "$SSH_KEY" "$SSH_USER@$VPLMN_IP" 'mkdir -p /home/alexis/TOPSSIM'
ssh -i "$SSH_KEY" "$SSH_USER@$UE_IP" 'mkdir -p /home/alexis/TestBed5G'

rsync -az --delete --exclude build --exclude logs ./open5gs/ \
  "$SSH_USER@$HPLMN_IP:/home/alexis/TOPSSIM/open5gs/"

rsync -az --delete --exclude build --exclude logs ./open5gs/ \
  "$SSH_USER@$VPLMN_IP:/home/alexis/TOPSSIM/open5gs/"

rsync -az --delete --exclude build --exclude logs ./open5gs/ \
  "$SSH_USER@$UE_IP:/home/alexis/TestBed5G/open5gs/"
```

If your SSH key is not the default key, add `-e "ssh -i $SSH_KEY"` to each `rsync` command.

## 6. Build Open5GS On Each VM

HPLMN:

```bash
ssh -i "$SSH_KEY" "$SSH_USER@$HPLMN_IP" \
  'cd /home/alexis/TOPSSIM/open5gs &&
   meson setup build --prefix="$PWD/install" --sysconfdir="$PWD/install/etc" --localstatedir="$PWD/install/var" &&
   ninja -C build &&
   test -x build/tests/app/5gc && echo HPLMN_OK'
```

VPLMN:

```bash
ssh -i "$SSH_KEY" "$SSH_USER@$VPLMN_IP" \
  'cd /home/alexis/TOPSSIM/open5gs &&
   meson setup build --prefix="$PWD/install" --sysconfdir="$PWD/install/etc" --localstatedir="$PWD/install/var" &&
   ninja -C build &&
   test -x build/tests/app/5gc && echo VPLMN_OK'
```

UE:

```bash
ssh -i "$SSH_KEY" "$SSH_USER@$UE_IP" \
  'cd /home/alexis/TestBed5G/open5gs &&
   meson setup build --prefix="$PWD/install" --sysconfdir="$PWD/install/etc" --localstatedir="$PWD/install/var" &&
   ninja -C build &&
   test -x build/tests/registration/registration && echo UE_OK'
```

## 7. Generate Cloud Open5GS Configs

Generate configs on the controller:

```bash
cd /path/to/POC/open5gs

python3 topssim/tools/make_three_vm_configs.py all \
  --cloud-single-ip \
  --hplmn-ip "$HPLMN_IP" \
  --vplmn-ip "$VPLMN_IP" \
  --ue-ip "$UE_IP" \
  --core-open5gs /home/alexis/TOPSSIM/open5gs \
  --ue-open5gs /home/alexis/TestBed5G/open5gs
```

Push the generated configs:

```bash
scp -i "$SSH_KEY" build/configs/examples/topssim-hplmn-cloud.yaml \
  "$SSH_USER@$HPLMN_IP:/home/alexis/TOPSSIM/open5gs/build/configs/examples/"

scp -i "$SSH_KEY" build/configs/examples/topssim-vplmn-cloud.yaml \
  "$SSH_USER@$VPLMN_IP:/home/alexis/TOPSSIM/open5gs/build/configs/examples/"

scp -i "$SSH_KEY" build/configs/examples/topssim-ue-cloud.yaml \
  "$SSH_USER@$UE_IP:/home/alexis/TestBed5G/open5gs/build/configs/examples/"
```

## 8. Prepare Campaign Env

Create a private env file on the controller:

```bash
cd /path/to/POC/open5gs
cp ../user_profile_campaign.cloud.env.template topssim/tools/user_profile_campaign.cloud.env
```

Edit `topssim/tools/user_profile_campaign.cloud.env`:

```text
SSH_USER=root
HPLMN_IP=<HPLMN_PUBLIC_IP>
VPLMN_IP=<VPLMN_PUBLIC_IP>
UE_IP=<UE_PUBLIC_IP>
LOCAL_OPEN5GS=/path/to/POC/open5gs
HOME_AMF_ADDR=<HPLMN_PUBLIC_IP>
VISITED_AMF_ADDR=<VPLMN_PUBLIC_IP>
REPETITIONS=20
SKIP_PDU_SESSION=0
```

If sudo is not passwordless, also set:

```text
VM_SUDO_PASSWORD=<sudo-password>
```

## 9. Validate Network And SSH

From the controller:

```bash
nc -vz "$HPLMN_IP" 22
nc -vz "$VPLMN_IP" 22
nc -vz "$UE_IP" 22
nc -vz "$HPLMN_IP" 7777
nc -vz "$HPLMN_IP" 7778
nc -vz "$VPLMN_IP" 7777
nc -vz "$VPLMN_IP" 7778
```

From the UE:

```bash
ssh -i "$SSH_KEY" "$SSH_USER@$UE_IP" \
  "timeout 8 sctp_test -H $UE_IP -P 0 -h $HPLMN_IP -p 38412 -s; echo RC:\$?"

ssh -i "$SSH_KEY" "$SSH_USER@$UE_IP" \
  "timeout 8 sctp_test -H $UE_IP -P 0 -h $VPLMN_IP -p 38412 -s; echo RC:\$?"
```

The SCTP test may time out because it is not a full NGAP peer. It should not fail immediately with routing/firewall errors.

## 10. Run A Smoke Campaign

```bash
cd /path/to/POC/open5gs

cp topssim/tools/user_profile_campaign.cloud.env \
   topssim/tools/user_profile_campaign.smoke.env

perl -0pi -e 's/^REPETITIONS=.*/REPETITIONS=1/m; s#^LOCAL_OUTPUT_DIR=.*#LOCAL_OUTPUT_DIR=logs/campaign-smoke#m' \
  topssim/tools/user_profile_campaign.smoke.env

SSH_KEY="$SSH_KEY" \
topssim/tools/run_user_profile_campaign.sh \
  topssim/tools/user_profile_campaign.smoke.env
```

Check that TOPSSIM uses the local SMF SDM-CF hit for `sm-data`:

```bash
rg -n "SMF-SDM-CF.*(SOURCE|HIT|MISS).*sm-data|nudm-sdm.*sm-data" \
  logs/campaign-smoke/topssim/run-1/vplmn.log
```

Expected:

- `SMF-SDM-CF][SOURCE] ... resource[sm-data] ... source[sdm-cf]`
- `SMF-SDM-CF][HIT] ... resource[sm-data]`
- no runtime `NF[SMF] service[nudm-sdm] ... sm-data` line in TOPSSIM.

## 11. Run The Full Campaign

```bash
cd /path/to/POC/open5gs

SSH_KEY="$SSH_KEY" \
topssim/tools/run_user_profile_campaign.sh \
  topssim/tools/user_profile_campaign.cloud.env
```

Final outputs:

```text
logs/campaign-pdu-20/user-profile-campaign-report.md
logs/campaign-pdu-20/user-profile-campaign-report.html
```

## 12. Cleanup Helpers

Stop stale Open5GS processes on HPLMN/VPLMN:

```bash
OPEN5GS_PROCS="5gc open5gs-nrfd open5gs-scpd open5gs-seppd open5gs-amfd open5gs-smfd open5gs-upfd open5gs-ausfd open5gs-udmd open5gs-udrd open5gs-pcfd open5gs-nssfd open5gs-bsfd"

for host in "$HPLMN_IP" "$VPLMN_IP"; do
  ssh -i "$SSH_KEY" "$SSH_USER@$host" \
    "for p in $OPEN5GS_PROCS; do sudo pkill -9 -x \"\$p\" || true; done"
done
```

Stop stale UE tunnels/tests:

```bash
ssh -i "$SSH_KEY" "$SSH_USER@$UE_IP" \
  "pkill -f 'ssh -.*127\\.10\\.0\\.|ssh -.*127\\.20\\.0\\.|build/tests/registration/registration' || true"
```
