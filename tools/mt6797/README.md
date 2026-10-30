# MT6797 image packaging

`mtk_trustzone_pack.py` packages a raw `bl31.bin` into the fixed-size image
expected by the MT6797 LK `tee1`/`tee2` partitions.  A raw BL31 must not be
flashed directly as `tee1`.

The packer takes an official open-board `trustzone.bin` as a template.  It
keeps the LK outer header and MTK TEE KTM header, replaces the code slot at
offset `0x440`, and zero-fills the unused part of the slot.  The inner
authentication area is intentionally preserved because the repository does
not contain the signing key or signer implementation.

The explicit acknowledgement below is required:

```sh
tools/mt6797/mtk_trustzone_pack.py \
  --template /path/to/trustzone.bin \
  --bl31 build/mt6797/debug/bl31.bin \
  --output /path/to/trustzone-mt6797-atf-bringup.bin \
  --allow-preserved-signature
```

This is bring-up-only packaging for the board reporting `secure: no`.  It is
not suitable for a secure production device until the inner signature is
regenerated with the correct MTK signing flow.
