# Hardware datasheet library

Manufacturer documentation for the requested WaveX hardware parts is stored in
[`../datasheets/`](../datasheets/). Use this index to select the document for the
exact component, package, temperature grade, and silicon revision being used.
Downloaded on **2026-09-16**; revisions below describe the saved copies.

## Module and processor references

| Local document | Revision | Download source |
| --- | --- | --- |
| [VisionSOM-RT117x datasheet and pinout](../datasheets/VisionSOM-RT117x_Datasheet_and_Pinout.pdf) | 20241115180840 | [SoMLabs](https://wiki.somlabs.com/extensions/JZPDFGen/pdf/VisionSOM-RT117x%20Datasheet%20and%20Pinout-720.pdf) |
| [RT1170 consumer datasheet — B silicon](../datasheets/IMXRT1170BCEC.pdf) | 1, May 2025 | [NXP](https://www.nxp.com/docs/en/data-sheet/IMXRT1170BCEC.pdf) |
| [RT1170 processor reference manual](../datasheets/IMXRT1170RM.pdf) | 3, November 2024 | [NXP document hosted by DigiKey](https://mm.digikey.com/Volume0/opasdata/d220001/medias/docus/6465/568_IMXRT1170RM%20manual%20REV3.pdf) |
| [RT1160/1170 hardware development guide](../datasheets/MIMXRT1170HDUG.pdf) | 2, September 2021 | [NXP Community attachment](https://community.nxp.com/pwmxy87654/attachments/pwmxy87654/imx-processors/208569/1/MIMXRT1170HDUG.pdf) |
| [RT1170B silicon errata](../datasheets/ES_IMXRT1170BCE.pdf) | 1.0, 10 June 2025 | [NXP](https://www.nxp.com/docs/en/errata/ES_IMXRT1170BCE.pdf) |

The selected module is `SLS14RT1176_800C_128R_16QSPI_0SF_E`. The retained
processor datasheet and errata assume consumer-grade B silicon
(`MIMXRT1176DVMAB`) for planning; the fitted processor remains unverified until
the backordered module arrives. Confirm its marking against the datasheet then.
Use the SoMLabs document for module connector details; MCU ball numbering is
not module connector numbering.

NXP's direct reference-manual and hardware-guide URLs did not provide PDFs
during this download. The saved reference manual is NXP's document from
DigiKey's mirror; the hardware guide is NXP's document hosted on NXP Community.

## Peripheral datasheets

| Requested component | Local document | Revision | Download source |
| --- | --- | --- | --- |
| MCP48CMB28-20E/ST | [MCP48CXBX4/8 family datasheet](../datasheets/MCP48CXBX4_8.pdf) | DS20006556B, July 2021 | [Microchip document hosted by DigiKey](https://media.digikey.com/pdf/Data%20Sheets/Microchip%20PDFs/MCP48CxBx4_8_2021.pdf) |
| TCA8418 | [TCA8418 datasheet](../datasheets/TCA8418.pdf) | SCPS215G, June 2018 | [Texas Instruments](https://www.ti.com/lit/ds/symlink/tca8418.pdf) |
| TLC5947 | [TLC5947 datasheet](../datasheets/TLC5947.pdf) | SBVS114B, January 2015 | [Texas Instruments](https://www.ti.com/lit/ds/symlink/tlc5947.pdf) |
| PCA9956BTWY | [PCA9956B datasheet](../datasheets/PCA9956B.pdf) | 1.2, 10 June 2020 | [NXP](https://www.nxp.com/docs/en/data-sheet/PCA9956B.pdf) |
| MCP3208 | [MCP3204/3208 datasheet](../datasheets/MCP3204_MCP3208.pdf) | DS21298E, 2008 | [Microchip](https://ww1.microchip.com/downloads/en/DeviceDoc/21298E.pdf) |

“TCL5947” in the request is interpreted as **TLC5947**, matching the TI part
and existing project documentation. Both requested LED-driver alternatives
are included. The PCA9956B ordering table explicitly lists PCA9956BTWY.

The MCP48CXBX4/8 family datasheet covers MCP48CMB28 and its 20-lead TSSOP
package. Revision B updates the TSSOP package drawing; this copy was obtained
from DigiKey because Microchip's reachable PDF was revision A and the attempted
revision-B links returned HTTP 403.

**Existing documentation discrepancy:** MCP48CMB28 is an **eight-channel,
12-bit DAC**, as shown in the datasheet's device comparison table. The
[architecture document's Stage-B discussion](../../docs/architecture.md)
describes it as dual-channel. Resolve that discrepancy before using the prose
to determine DAC quantities or connectivity.

## Verification and updates

All 10 retained files were checked for a PDF signature, parsed with `pdfinfo`, and had
their identifying text inspected with `pdftotext`. These checks establish
document identity and readability; they do not validate a circuit or select
a replacement component.

The PDFs are unchanged vendor documents and retain their original copyright
notices. The project's software license does not replace those notices.
The download-source column records the actual source of each saved copy.
[`SHA256SUMS`](../datasheets/SHA256SUMS) records their downloaded bytes. From the
repository root, verify the local copies with:

```sh
cd hardware/datasheets
sha256sum -c SHA256SUMS
```

When updating a PDF, check its document ID, revision, and applicable parts,
then update this index and its checksum together.

## Related

- [SoMLabs VisionSOM-RT117x documentation hub](https://wiki.somlabs.com/index.php/VisionSOM-RT117x)
- [Live SoMLabs module datasheet and pinout](https://wiki.somlabs.com/index.php/VisionSOM-RT117x_Datasheet_and_Pinout)
- [Hardware working instructions](../AGENTS.md)
- [Canonical WaveX architecture](../../docs/architecture.md)
