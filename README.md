# BB10 VoLTE Addon

**Enable VoLTE on BlackBerry 10 devices.**

An addon designed to enable and improve Voice over LTE (VoLTE) functionality on supported BlackBerry 10 devices, helping extend their usability as mobile carriers phase out legacy 2G and 3G networks.

![Platform](https://img.shields.io/badge/Platform-BlackBerry%2010-000000?logo=blackberry)
![OS](https://img.shields.io/badge/OS-10.3.x-blue)
![Requirements](https://img.shields.io/badge/Requires-AutoRoot%20v4%2B-orange)
![Status](https://img.shields.io/badge/Status-Experimental-yellow)

## 📌 About

As mobile operators continue to retire older cellular networks, devices that rely on 2G or 3G for voice calls may lose their ability to make and receive calls, even when LTE data connectivity remains available.

**BB10 VoLTE Addon** aims to address this limitation by enabling VoLTE functionality on supported BlackBerry 10 hardware.

The project builds upon community research into the BlackBerry 10 VoLTE implementation and provides an installable addon distributed as a ZIP archive.

> **Note:** VoLTE availability depends on device hardware, firmware, carrier configuration, network compatibility, and operator provisioning. Installing this addon does not guarantee that VoLTE will work on every device or network.

## ✨ Features

- **VoLTE Enablement** — enables VoLTE-related functionality on supported BlackBerry 10 devices.
- **LTE Voice Calling** — aims to allow voice calls over LTE without relying on legacy 2G/3G networks.
- **Extended Device Usability** — helps keep compatible BB10 devices useful as legacy cellular networks are decommissioned.
- **Easy Installation** — distributed as a ZIP package and installed through AutoRoot.
- **Community Research** — builds upon existing reverse engineering and proof-of-concept research from the BB10 community.

## 📱 Compatibility

### Requirements

- A supported BlackBerry 10 device.
- BlackBerry 10 OS **10.3.x**.
- **AutoRoot v4 or newer** installed on the device.
- An active SIM card and a mobile subscription with VoLTE enabled.
- A carrier and LTE network compatible with the device's VoLTE implementation.

### Important compatibility notes

VoLTE support varies between devices, firmware versions, and mobile operators.

Even if LTE data works correctly, voice over LTE may not work unless the carrier accepts the device's configuration and provisions the service for it.

Compatibility should be verified on each target device and network.

### Carrier provisioning

The addon keeps its own carrier list and builds the VoLTE profile for your SIM on the phone, so most carriers need no per-carrier file.

**Carrier list.** The stock list dates from 2017. On first start, and when its copy is over 30 days old, the addon downloads Android's carrier and APN lists from `android.googlesource.com`. It adds the missing carriers to a copy of the stock list and links that copy over `/base/etc/carrier/ids/carrierIds.xml`. The system partition is not modified, and the link is made again at every start. The files live in `data/`: `carrierIds.xml`, `carrierIds.stock.xml` and `carrier-apns.tsv`.

The download needs a working connection. If the phone has no working APN yet, connect to Wi-Fi or set the APN by hand once. If the download fails, the stock list stays in use and the addon tries again at the next start. To refresh the list by hand, run `ims_manager -a provision`.

**Profile.** The addon uses `data/carrier-<carrier_id>-volte.xml` if you wrote one. Otherwise it builds `data/carrier-auto-<carrier_id>-volte.xml` at start, from the stock config for that carrier, or from the stock default config plus Android's APNs. It adds an IMS service unless the stock config already has a real one.

Android's APNs replace the stock ones, which are around ten years old. A stock APN stays only if Android has no usable APN for that carrier, or doesn't give one clear answer across the networks that carrier spans. Android APNs that are IPv6-only or can't be used on LTE are skipped.

The profile is linked over `/base/etc/carrier/config/<carrier_id>.xml` and published as the carrier's normal config, so you can change the APN in Settings. A carrier with no stock config and no Android APN starts with it empty: enter it in Settings, because a guessed APN can break mobile data.

**SIMs with no carrier ID.** A SIM that is in neither list has no profile to link. It can only use an optional `data/carrier-default-volte.xml`, which also sets the APNs. The phone can't save APN changes while it uses this file. To add it, create the file and add this line to `pkg/data/perms`:

```text
carrier-default-volte.xml:0:0:644
```

### Forcing IMS voice (experimental)

Some networks register the device but report no IMS voice support, so calls fall back to 2G/3G. If your network does accept the calls, the addon can make outgoing calls use VoLTE anyway: add an empty `pkg/data/force-ims-voice` file and list it in `pkg/data/perms`:

```text
force-ims-voice:0:0:644
```

While IMS is registered on LTE, the daemon then sets the `IMS_VOICE_SUPPORTED` bit that `ims_rrm` checks. Only use this if you have tested it on your network: if the network refuses these calls, they may fail instead of falling back. Incoming calls are still routed by the network, so they may keep using 2G/3G. Delete the file from `data/` on the device to turn this off, because reinstalling does not remove it.

## 🚀 Installation

### 1. Download the addon

Download or build the `volte.zip` archive from the project's release or distribution source.

### 2. Transfer the archive

Copy `volte.zip` to an accessible directory on your BlackBerry 10 device.

For example, you may place it in a temporary directory or another location accessible to the installer.

### 3. Open a terminal

Open a terminal directly on the device or connect remotely using SSH or Telnet, if available.

### 4. Run the installer

Execute the AutoRoot installer with the path to the ZIP archive:

```bash
/var/rootdata/installer /path/to/volte.zip
```

Replace `/path/to/volte.zip` with the actual path to the transferred archive.

When upgrading, stop the daemon first and wait until it has exited. Otherwise the installer can report success without replacing the binary:

```bash
slay ims_manager
pidin arg | grep '[i]ms_manager'   # repeat until this prints nothing
```

### 5. Verify functionality

After installation:

1. Follow any instructions displayed by the installer.
2. Reboot the device if required by the installation process.
3. Verify that LTE connectivity is available.
4. Check whether VoLTE is registered or enabled, where the device exposes this information.
5. Test incoming and outgoing calls while the device is connected to LTE.

**Before installing:** Back up important data and ensure you have a way to recover the device if the addon causes unexpected behavior.

## ⚠️ Disclaimer

This project is provided for research, experimentation, and preservation of BlackBerry 10 devices.

- VoLTE functionality is not guaranteed on all devices or carriers.
- Carrier-side provisioning and network restrictions may prevent calls over LTE.
- Installation may modify system files or configuration.
- Incorrect modifications may affect device stability or cellular connectivity.
- Use this addon at your own risk.

Always maintain a backup of important data and a recovery option before modifying system components.

## 🙏 Credits & Acknowledgments

This project would not be possible without the BlackBerry 10 community and its ongoing research into the platform's cellular capabilities.

Special thanks to:

- **[Qin Yu (imqinyu)](https://github.com/imqinyu)** — for the core VoLTE research and proof-of-concept work that inspired this project.
- **[Hazematman](https://github.com/Hazematman)** — for the initial findings, research, and technical insights that helped make further investigation possible.

### Research references

This project is based on and inspired by the following research:

- [BB10DeviceVoLTEResearch — Qin Yu](https://github.com/imqinyu/BB10DeviceVoLTEResearch)
- [Hazematman's BlackBerry 10 research](https://github.com/Hazematman)

Please refer to the original research for technical background and attribution.

## 📄 License

This project is licensed under the **GNU General Public License v3.0 (GPL-3.0)**.

You are free to use, study, modify, and redistribute this software under the terms of the license.

See the [LICENSE](LICENSE) file for the full license text.

---

<p align="center">
  <b>Keeping BlackBerry 10 connected.</b><br>
  Built on community research, reverse engineering, and a passion for BB10.
</p>
