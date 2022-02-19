## Example Summary

This example demonstrate the Over the Air (OTA) update from remote servers on CC32xx devices. 
The OTA update is a bundle of files packed in archive tar file and may include:
	- Service pack for the NWP
	- New MCU image
	- Other user files

## Peripherals Exercised

* `Board_LED0 (red LED for cc3220, blue LED for cc3235)` - Indicates that the board was initialized within `main()`
and while blinking, indicates that the board is trying to connect to AP.

* `LaunchPad board button` - Stop Pings Start the OTA update process, 
can be disabled by defining DISABLE\_OTA\_SWITCH\_TRIGGER in cloud_ota.c (5 Pings and then OTA process)


## Example Usage

* Downloads and Installations.
	> Make sure you have Uniflash installed and Code Composer Studio or IAR IDE.

* !! IMPORTANT NOTE!! 
	- User must add access token (Dropbox, Github or custum) and define the OTA_VENDOR_TOKEN in otauser.h 
        `#define OTA_VENDOR_TOKEN     "<access token>"`
   		> Further information and example on how to define the vendor token can be found [here](../../../../../docs/simplelink_mcu_sdk/Over_The_Air_Update_Guide.html#fig.-20:-OTA-tokan-Diagram)
	- if `OTA_SERVER_TYPE` is defined as `OTA_FILE_DOWNLOAD` - the CDN negotiation will be skipped, sending the library directly to the tar file download. A server certificate (`OTA_SERVER_ROOT_CA_CERT`) will need to be defined in `otauser.h`. The HTTP server URL should be set to a global variable (`g_OtaTarFileURL` defined in the `cloud_ota.c`).

* Checklist of required elements.
	- Uniflash projects according to device flavor.	
	- Certificate artifacts, ca\_in\_cert\_store, cert-good.der certificate and cert-good-key.der private key.
	- Certificate for dropbox and github: DigCert\_High\_Assurance\_CA.der, DigiCcert\_global\_root\_ca.der (both certificates are included in RootCACerts.pem which is available as part of the OTA library sources).
	> NOTE: Both DropBox and GitHub are in the process of updating their servers' certificates which will require an updated of the related root CA certificate in the device (having the new certificate in the file system and updating the otauser.h).<br>
	The existing DigCert\_High\_Assurance\_CA.der is about to be replaced with DigiCcert\_global\_root\_ca.der.<br>
	CC3220 users should update the certificates just before the change happens.<br>
    CC323x user can use the RootCACerts.pem which contain both the old and new certifcates and will work before and after the Servers' certificate update. 
    See more details in otauser.h
	 
	- Simple link provisioning application on your mobile device
	- archive tar file for device update should be placed in the Dropbox or Github servers.

* Build the setup.
	> Make sure you have the a local AP.

* Build the project and flash it by using the SimpleLink Uniflash utility.
	> The example could be executed from the debugger as well but since it involves platform reset at the end 
        (so the new OTA image can be tested and committed),the debugger would get disconnected.
	> The example could be downloaded by the ImageCreator, make sure that all the certificates file are there 

* Open a serial session (e.g. `HyperTerminal`,`puTTY`, etc.) to the appropriate COM port.
	> The COM port can be determined via Device Manager in Windows or via `ls /dev/tty*` in Linux.

The connection should have the following settings
```
    Baud-rate:    115200
    Data bits:       8
    Stop bits:       1
    Parity:       None
    Flow Control: None
```

* Run the example by pressing the reset button. The example should start running automatically when programming via Uniflash is done.

	*  The example will start connection by the provisioning process
		- Follow provisioning application instructions in order to connect to your local AP
	*  The example will start pinging external server

* At this point, Cloud OTA is enabled and may be triggered by pressing the LaunchPad button

* The procedure to update a new version is as follows:
	> OTA process will connect the Dropbox/Github CDN server to get the archive tar file 
	- The Dropbox/Github account and directory is configured from otauser.h file
	- This step will be skipped in case `OTA_FILE_DOWNLOAD` is configured 
	> OTA will save all archive files in the NWP SFLASH
	> OTA, on download done, will call sl_Stop() and reset the MCU in order to move the new update into 'TESTING' mode
	> OTA, after reset, will run the new update and on successful ping (system check is ok) will commit the new update 
	> OTA will try again to download only newer update from the CDN server

## Application Design Details

This example demonstrates remote Over-the-Air update experience with CC32xx LaunchPad development kit.
Over-the-Air update demonstrates an update of a full image over a secured channel. OTA service enables in-system update of the MCU application, CC32xx firmware releases (a.k.a. Service Pack) made available by Texas Instruments and other vendor files. An update procedure executed in a full system integrity fashion, i.e. failure in upgrading any of the image components would lead to rolling back to the previous valid version.

> Please install the latest CC32xx Service Pack.

## References

* For full detailed user guide see [Over The Air Update Guide](../../../../../docs/simplelink_mcu_sdk/Over_The_Air_Update_Guide.html)
* For further information please refer to the user programmers guide: [CC3X20 Programmer's Guide](http://www.ti.com/lit/swru455)
 
