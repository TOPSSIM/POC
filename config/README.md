# Config Files

This directory intentionally does not include machine-specific generated YAML files.

Generate the cloud configs after the colleague knows their VM public IPs:

```bash
cd /path/to/open5gs

python3 topssim/tools/make_three_vm_configs.py all \
  --cloud-single-ip \
  --hplmn-ip <HPLMN_PUBLIC_IP> \
  --vplmn-ip <VPLMN_PUBLIC_IP> \
  --ue-ip <UE_PUBLIC_IP> \
  --core-open5gs /home/alexis/TOPSSIM/open5gs \
  --ue-open5gs /home/alexis/TestBed5G/open5gs
```

The generated files are:

- `build/configs/examples/topssim-hplmn-cloud.yaml`
- `build/configs/examples/topssim-vplmn-cloud.yaml`
- `build/configs/examples/topssim-ue-cloud.yaml`

Those files are platform-specific because they embed the public AMF/SEPP endpoints and local Open5GS paths.

