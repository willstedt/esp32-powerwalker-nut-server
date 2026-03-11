# Om projektet
Konverterar en ESP32-S3 till en NUT-server för Powerwalker UPS.  Då kan Unifi läsa UPS-data och alla datorer i nätverket kan ta del av UPS-information.

**Viktigt:** Du behöver löda +5V från ESP32:an till de stora benen till höger eller vänster på ESP32:ans högra USB-port eftersom ESP32 inte skickar ut 5V till USB-porten (den är USB-slav och det är bara USB-masters som skickar ut 5V). Därför behöver du ta 5V från ESP32 och skicka ut på ena benet.

# Vad som behövs
Du behöver ha en ESP32-S3 med dubbla USB-portar  
Den väntra USB-porten används för att ge ström och ladda upp projekt-uppdateringar.  
Den högra USB-porten används för att läsa data från Powerwalkern.
