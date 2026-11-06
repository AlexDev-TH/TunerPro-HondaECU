# ตารางรุ่นกล่อง

[กลับหน้าหลัก](../README.md) | [English](../README.en.md)

ตารางนี้คัดมาจาก README ของ HondaECU 3.3.0 ไม่ได้แก้ค่า และยังไม่ได้ตรวจกับกล่องจริงทุกรุ่น ก่อนใช้ค่าไหนควรเทียบกับไฟล์ของรุ่นนั้นด้วย

This table is copied from the HondaECU 3.3.0 README with no changes to the values. Not every model has been checked on a real ECU.

- **Checksum Address** และ **Start Offset** เป็นเลขฐาน 16 ถ้าเอาไปใส่ในปลั๊กอินให้ใส่เป็น `0x8000` เป็นต้น
- ช่องว่างหมายถึงต้นฉบับไม่ได้ระบุไว้
- **Pinout** คือขาบนขั้วกล่อง เรียงเป็น กราวด์, +12V, K-Line

ถ้าปลั๊กอินรู้จักรหัสกล่อง จะเติม Start Offset และ Checksum Location ให้เอง รายการที่ปลั๊กอินรู้จักอยู่ใน [`src/ecmids.inc`](../src/ecmids.inc) ถ้าไม่รู้จัก หน้าต่างจะขึ้นว่า Unknown Model แต่ยังใช้งานได้ โดยต้องใส่ค่าเอง

| Model | Year | ROM Size | Keihin Code | Checksum Address | Start Offset | CPU | Pinout (Gnd,+12,Kline) |
|---|---|---|---|---|---|---|---|
| Z125M | 2019 | 92 kb | 38770-K0F-A01 | 0 | 8000 | M16C | 10,1,30 |
| CRF110F | 2019 | 64 kb | 38770-KYK-D12 | 0 | 8000 | M16C | 10,1,30 |
| MSX125 | 2017 | 64 kb | 38770-K26-B13 | 0 | 8000 | M16C | 10,1,30 |
| MSX125 | 2014 | 48 kb | 38770-K26-911 | 0 | 4000 | M16C | 10,1,30 |
| CBR250R | 2012 | 56 kb | 38770-KYJ-971 | DFEF | | | |
| CBR250R HRC | 2014 | 256 kb | 38770-K33-R51 | 18FFE | | | |
| CBR250RR HRC | 2017 | 1024 kb | 38770-K64-R02 | 7FFF8 | | | |
| CRF250R | 2011 | 256 kb | 38770-KRN-E52 | 1FFFA | | | |
| CRF250R | 2013 | 256 kb | 38770-KRN-E73 | 18FFE | | | |
| CRF250R | 2014 | 256 kb | 38770-KRN-E82 | 19FFE | | | |
| CRF250R | 2015 | 256 kb | 38770-KRN-E92 | 19FFE | | | |
| NSF250R | 2013 | 256 kb | 38770-NX7-033 | 18FFE | | | |
| VTR250 | 2015 | 256 kb | 38770-KFK-632 | 3FFF8 | | | |
| CBR300R HRC | 2014 | 256 kb | 38770-K33-R01 | 18FFE | | | |
| CB300 | 2008-2012 | 56 kb | 38770-KVK-xxx | DFEF | | | |
| SH300 | 2009 | 56 kb | 38770-KTW-901 | DFEF | | | |
| XRE 300 | 2009-2012 | 56 kb | 38770-KWT-xxx | DFEF | | | |
| CRF450R | 2009 | 256 kb | 38770-MEN-E21 | 1FFFA | | | |
| CRF450R | 2011 | 256 kb | 38770-MEN-E52 | 1FFFA | | | |
| CRF450R | 2014 | 256 kb | 38770-MEN-A73 | 19FFE | | | |
| CB500R | 2014 | 256 kb | 38770-MGZ-B01 | 3FFF8 | | PPC | 12,23,25 |
| CBR600RR | 2007-2016 | 256 kb | 38770-Mxx-xxx | 3FFF8 | | M32R | |
| CB600F | 2010 | 256 kb | 38770-MGM-B11 | 3FFF8 | | | |
| CB650F | 2015 | 256 kb | 38770-MJE-B41 | 3FFF8 | | | |
| CB600F/R | 2010-2011 | 256 kb | 38770-MFG-Bxx- | 3FFF8 | | | |
| Transalp700 | 2013 | 256 kb | 38770-MFF-B01 | 3FFF8 | | | |
| Shadow 750 | 2009 | 256 kb | 38770-MGE-B21 | 3FFF8 | | | |
| NC750X | 2015 | 256 kb | 38770-Mxx-xxx | 3FFF8 | | | |
| NC750S | 2016 | 512 kb | 38770-MJL-D72 | 7FFF8 | | | |
| VFR800F | 2015 | 512 kb | 38770-MJM-J13 | 7FFF8 | | | |
| CB1000R | 2008-2016 | 256 kb | 38770-Mxx-xxx | 3FFF8 | | | |
| CBR1000RR | 2004-2005 | 256 kb | 38770-MEL-xxx | 3FFFC | | M32R | |
| CBR1000RR | 2006-2016 | 256 kb | 38770-Mxx-xxx | 3FFF8 | | M32R | |
| CBR1000RR | 2017 | 1024 kb | 38770-MKFA-D72 | FFFF8 | | | |
| VFR1200F | 2010 | 1024 kb | 38770-MGE-D02 | 7FFF8 | | | |
| GL1800 | 2015 | 256 kb | 38770-MJK-J21 | 3FFF8 | | | |
