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
