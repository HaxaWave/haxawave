# Modem firmware for sample streaming (MSM8916)

Builds the modem firmware from the public `modem_proc` source drop, with a few
small patches, and installs it on a phone running Nura (formerly postmarketOS)
so that the modem can be switched to it on demand. Tested on the Wileyfox Swift
(MSM8916).

Abbreviations are explained in the [glossary](../README.md#glossary).

## The patches (`patches/`)

`build.sh` applies them in order. Each one is a single change with a description
at its top.

- **`0001-build-with-hexagon-tools-6.4.06a`**: build with Hexagon tools 6.4.06.a
  (`build/ms/build_cfg.xml`: tools release, compiler warnings are not errors).
- **`0002-no-q6zip-keep-rf-card-code-resident`**: RF card code and data stay in
  the normal sections (`build/ms/dynrec.lst`, the `.lcs` file). With the Q6ZIP
  lists emptied by `build.sh` (`config/*/ms/compress.lst`) no code is
  compressed.
- **`0003-88mib-physical-pool`**: the physical pool grows to 88 MiB
  (`cust_config.xml`), matching the 88 MiB modem region of the kernel in
  `../pmos`.
- **`0004-soc-id-msm8916`**: the chip reports SoC id 0x2000 (MSM8916)
  (`config/8936.genns/ms/image_cfg.xml`).
- **`0005-rfc-optional-qfe3320-family`**: the `china_v2` RF card lists a QFE3320
  front-end module (power amplifier and switches) that some boards do not carry;
  those parts become optional instead of failing RF initialisation (`rfc.cpp`).
- **`0006-rfc-lte-grfc-pa-only-with-pa-on`**: the generic power amplifier
  switched by GRFC lines (RF control pins of the modem chip) is added only to
  LTE bands that have a `PA_ON` signal (`rfc_lte.cpp`).
- **`0007-rf-sandbox-task`**: the `rf_sandbox` task through which the host runs
  code on the modem DSP (`rfa/rf/task/common/`), and a writable IMEM mapping for
  its address table (`config/8916/ms/cust_config.xml`).

### The `rf_sandbox` task

The sample copy loop (pool memory -> DDR through the MCDMA engine) runs on the
modem DSP, so something has to start code there. The task does exactly one
thing: it calls code that the application processor (AP) supplies. It exposes a
control block (`rf_sandbox_ctrl`) and a 2 KiB scratch area in an executable
segment. The addresses are published in IMEM at `0x08600F00` (magic `SBFR`),
which the host reads through `/dev/mem`. The AP writes the code into the scratch
area and the arguments into the control block (`cmd = 29`, `arg0` = address of
the function, `arg1` and `data_words[0..1]` = its three arguments), then
increments `seq`. The task calls `fn(arg1, data_words[0], data_words[1])`,
stores the return value in `result` and sets `ack = seq`. Only addresses inside
the scratch area are accepted. `cmd = 0` is a no-operation that is only
acknowledged (a liveness check). Memory and register access is not part of the
task: the AP reads and writes memory through `/dev/mem`, and code that has to
touch DSP-side registers is itself a program run through `cmd = 29` (see
`../sdr/dsp`).

The task sleeps on a non-deferrable timer (`timer_non_defer_group`).
`rex_sleep()` uses a deferrable timer, which only fires when something else
wakes the sleeping modem DSP (tens of seconds when the modem is idle); with the
non-deferrable timer the task wakes once per `period_ms` and a host command is
acknowledged within about a second. `period_ms` (default 1000) is a plain field
of the control block that the AP may change at any time; the task reads it
before every sleep, and a task counter (`rf_sandbox_task_counter`, address in
the IMEM table) increments at every wake-up.

## Build

```
bash build.sh [WORKDIR]
```

Downloads `modem_proc.zip` from GitLab
(qcom-sources15/msm8939_2014-12-08_amss_oem, checked against a fixed sha256) and
the Hexagon tools (a third-party copy on GitHub, pinned to one commit and
checked against its sha256; see `HEXAGON_COMMIT` and `HEXAGON_SHA256` in
`build.sh`), applies the patches, builds a Docker image from `docker/Dockerfile`
and builds the firmware inside it. Result: `WORKDIR/out/pil/modem.mdt` and
`modem.bNN`. If you already have the sources or the tools:
`MODEM_PROC_ZIP=/path/modem_proc.zip HEXAGON_TOOLS=/path/to/HEXAGON_Tools bash build.sh`
(`HEXAGON_TOOLS` is the directory that contains `6.4.06.a/`).

## Install on the phone

```
bash ../pmos/install_config.sh                  # once: installs qcom-sdr-switch-fw and the other helpers
bash install_firmware.sh WORKDIR/out/pil        # copies the image to /var/lib/qcom-sdr/modem (asks for the passwords)
ssh -t user@172.16.42.1 'sudo qcom-sdr-switch-fw custom force'    # restart the modem with the custom firmware
ssh -t user@172.16.42.1 'sudo qcom-sdr-switch-fw stock'     # back to the firmware from the modem partition
```

The phone stays on the stock firmware after a reboot unless you enable
`qcom-sdr-modem-fw.service` (`sudo systemctl enable qcom-sdr-modem-fw.service`),
which loads the custom image at every boot. With the custom image loaded at boot
the modem is not restarted while the phone runs, so the audio module blacklist
(see `../pmos`) is not needed and sound works normally. Every restart of the
modem is a small risk: after about ten in a row it may fail to boot
(`PBL boot timed out` in `dmesg`); then reboot the phone.

Check that the custom firmware is running (on the phone, as root; copy
`check_sandbox.py` there first):

```
sudo python3 check_sandbox.py ping      # prints the control block and the task counter, then waits for the PING acknowledgement
```

The diagnostic channel of the modem (`/dev/wwan0qcdm0`, needed to put the modem
into factory test mode) requires the patched kernel module from `../pmos` (patch
0003 and `install_config.sh`).
