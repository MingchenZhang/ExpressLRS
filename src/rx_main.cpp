#include <Arduino.h>
#include "targets.h"
#include "utils.h"
#include "common.h"
#include "LoRaRadioLib.h"
#include "CRSF.h"
#include "FHSS.h"
// #include "Debug.h"
#include "rx_LinkQuality.h"
#ifdef PLATFORM_ESP8266
#include "ESP8266_WebUpdate.h"
#endif

SX127xDriver Radio;
CRSF crsf(Serial); //pass a serial port object to the class for it to use

///forward defs///
void SetRFLinkRate(expresslrs_mod_settings_s mode);
void InitOStimer();
void OStimerSetCallback(void (*CallbackFunc)(void));
void OStimerReset();
void OStimerUpdateInterval(uint32_t Interval);

void InitHarwareTimer();
void StopHWtimer();
void HWtimerSetCallback(void (*CallbackFunc)(void));
void HWtimerSetCallback90(void (*CallbackFunc)(void));
void HWtimerUpdateInterval(uint32_t Interval);
uint32_t ICACHE_RAM_ATTR HWtimerGetlastCallbackMicros();
uint32_t ICACHE_RAM_ATTR HWtimerGetlastCallbackMicros90();
void ICACHE_RAM_ATTR HWtimerPhaseShift(int16_t Offset);
uint32_t ICACHE_RAM_ATTR HWtimerGetIntervalMicros();

uint8_t scanIndex = 1;

uint8_t prevAirRate = 0;
uint8_t currAirRate = 0;

//uint32_t HWtimerLastcallback;
uint32_t MeasuredHWtimerInterval;
int32_t HWtimerError;
int32_t HWtimerError90;
int16_t Offset;
int16_t Offset90;
uint32_t SerialDebugPrintInterval = 250;
uint32_t LastSerialDebugPrint = 0;

uint32_t RFmodeLastCycled = 0;
uint32_t RFmodeCycleInterval = 500;

uint8_t testdata[7] = {1, 2, 3, 4, 5, 6, 7};

bool LED = false;

bool buttonPrevValue = true; //default pullup
bool buttonDown = false;     //is the button current pressed down?
uint32_t buttonSampleInterval = 150;
uint32_t buttonLastSampled = 0;
uint32_t buttonLastPressed = 0;
uint32_t webUpdatePressInterval = 2000; //hold button for 2 sec to enable webupdate mode
uint32_t buttonResetInterval = 4000;    //hold button for 4 sec to reboot RX
bool webUpdateMode = false;

uint32_t webUpdateLedFlashInterval = 25;
uint32_t webUpdateLedFlashIntervalLast;

volatile uint8_t NonceRXlocal = 0; // nonce that we THINK we are up to.

//////////////////////////////////////////////////////////////

///////Variables for Telemetry and Link Quality///////////////
int packetCounter = 0;
int CRCerrorCounter = 0;
float CRCerrorRate = 0;
uint32_t PacketRateLastChecked = 0;
uint32_t PacketRateInterval = 1000;

float PacketRate = 0.0;

uint32_t LostConnectionDelay = 1000; //after 1500ms we consider that we lost connection to the TX
bool LostConnection = true;
bool gotFHSSsync = false;
uint32_t LastValidPacket = 0; //Time the last valid packet was recv
///////////////////////////////////////////////////////////////

void ICACHE_RAM_ATTR GenerateSyncPacketData()
{
    uint8_t PacketHeaderAddr;
    PacketHeaderAddr = (DeviceAddr << 2) + 0b10;
    Radio.TXdataBuffer[0] = PacketHeaderAddr;
    Radio.TXdataBuffer[1] = FHSSgetCurrIndex();
    Radio.TXdataBuffer[2] = NonceRXlocal;
    Radio.TXdataBuffer[3] = ExpressLRS_currAirRate.enum_rate;
}

void ICACHE_RAM_ATTR getRFlinkInfo()
{
    int8_t LastRSSI = Radio.GetLastPacketRSSI();
    linkQuality = getRFlinkQuality();

    crsf.PackedRCdataOut.ch15 = UINT10_to_CRSF(map(LastRSSI, -100, -50, 0, 1023));
    crsf.PackedRCdataOut.ch14 = UINT10_to_CRSF(fmap(linkQuality, 0, 100, 0, 1023));

    crsf.LinkStatistics.uplink_RSSI_1 = 120 + Radio.GetLastPacketRSSI();
    crsf.LinkStatistics.uplink_RSSI_2 = 0;
    crsf.LinkStatistics.uplink_SNR = Radio.GetLastPacketSNR() * 10;
    crsf.LinkStatistics.uplink_Link_quality = linkQuality;

    //crsf.sendLinkStatisticsToFC();
}

int offset = 0;

void ICACHE_RAM_ATTR HandleFHSS()
{
    uint8_t modresult = (NonceRXlocal - offset) % ExpressLRS_currAirRate.FHSShopInterval;

    if (modresult == 0)
    {
        if (LostConnection == false) // don't hop if we lost
        {
            Radio.SetFrequency(FHSSgetNextFreq());
            Radio.RXnb();
        }
    }
}

void ICACHE_RAM_ATTR HandleSendTelemetryResponse()
{
    if (ExpressLRS_currAirRate.TLMinterval > 0)
    {
        uint8_t modresult = (NonceRXlocal - offset) % ExpressLRS_currAirRate.TLMinterval;

        if (modresult == 0)
        {
            Radio.TXdataBuffer[0] = (DeviceAddr << 2) + 0b11; // address + tlm packet
            Radio.TXdataBuffer[1] = CRSF_FRAMETYPE_LINK_STATISTICS;
            Radio.TXdataBuffer[2] = crsf.LinkStatistics.uplink_RSSI_1;
            Radio.TXdataBuffer[3] = 0;
            Radio.TXdataBuffer[4] = crsf.LinkStatistics.uplink_SNR;
            Radio.TXdataBuffer[5] = crsf.LinkStatistics.uplink_Link_quality;

            uint8_t crc = CalcCRC(Radio.TXdataBuffer, 7) + CRCCaesarCipher;
            Radio.TXdataBuffer[7] = crc;
            Radio.TXnb(Radio.TXdataBuffer, 8);
            crsf.sendLinkStatisticsToFC();
            // Serial.println("TLM");
            addPacketToLQ(); // Adds packet to LQ otherwise an artificial drop in LQ is seen due to sending TLM.
        }
    }
}

//expresslrs packet header types
// 00 -> standard 4 channel data packet
// 01 -> switch data packet
// 11 -> tlm packet
// 10 -> sync packet with hop data

void ICACHE_RAM_ATTR Test90()
{
    incrementLQArray();
    NonceRXlocal++;
    HandleFHSS();
    HandleSendTelemetryResponse();
}

void ICACHE_RAM_ATTR Test()
{
    MeasuredHWtimerInterval = micros() - HWtimerGetlastCallbackMicros();
}

///////////Super Simple Lowpass for 'PLL' (not really a PLL)/////////
int RawData;
int32_t SmoothDataINT;
int32_t SmoothDataFP;
int Beta = 3;     // Length = 16
int FP_Shift = 3; //Number of fractional bits

int16_t ICACHE_RAM_ATTR SimpleLowPass(int16_t Indata)
{
    RawData = Indata;
    RawData <<= FP_Shift; // Shift to fixed point
    SmoothDataFP = (SmoothDataFP << Beta) - SmoothDataFP;
    SmoothDataFP += RawData;
    SmoothDataFP >>= Beta;
    // Don't do the following shift if you want to do further
    // calculations in fixed-point using SmoothData
    SmoothDataINT = SmoothDataFP >> FP_Shift;
    return SmoothDataINT;
}
//////////////////////////////////////////////////////////////////////

void ICACHE_RAM_ATTR GotConnection()
{
    if (LostConnection)
    {
        HWtimerUpdateInterval(ExpressLRS_currAirRate.interval);
        InitHarwareTimer();
        LostConnection = false; //we got a packet, therefore no lost connection
        Serial.println("got conn");
        Beta = 4;
    }
}

void ICACHE_RAM_ATTR UnpackChannelData_11bit()
{
    crsf.PackedRCdataOut.ch0 = (Radio.RXdataBuffer[1] << 3) + (Radio.RXdataBuffer[5] & 0b11100000 >> 5);
    crsf.PackedRCdataOut.ch1 = (Radio.RXdataBuffer[2] << 3) + (Radio.RXdataBuffer[5] & 0b00011100 >> 2);
    crsf.PackedRCdataOut.ch2 = (Radio.RXdataBuffer[3] << 3) + (Radio.RXdataBuffer[5] & 0b00000011 << 1) + (Radio.RXdataBuffer[6] & 0b10000000 >> 7);
    crsf.PackedRCdataOut.ch3 = (Radio.RXdataBuffer[4] << 3) + (Radio.RXdataBuffer[6] & 0b01110000 >> 4);
#ifdef One_Bit_Switches
    crsf.PackedRCdataOut.ch4 = BIT_to_CRSF(Radio.RXdataBuffer[6] & 0b00001000);
    crsf.PackedRCdataOut.ch5 = BIT_to_CRSF(Radio.RXdataBuffer[6] & 0b00000100);
    crsf.PackedRCdataOut.ch6 = BIT_to_CRSF(Radio.RXdataBuffer[6] & 0b00000010);
    crsf.PackedRCdataOut.ch7 = BIT_to_CRSF(Radio.RXdataBuffer[6] & 0b00000001);
#endif
}

void ICACHE_RAM_ATTR UnpackChannelData_10bit()
{
    crsf.PackedRCdataOut.ch0 = UINT10_to_CRSF((Radio.RXdataBuffer[1] << 2) + (Radio.RXdataBuffer[5] & 0b11000000 >> 6));
    crsf.PackedRCdataOut.ch1 = UINT10_to_CRSF((Radio.RXdataBuffer[2] << 2) + (Radio.RXdataBuffer[5] & 0b00110000 >> 4));
    crsf.PackedRCdataOut.ch2 = UINT10_to_CRSF((Radio.RXdataBuffer[3] << 2) + (Radio.RXdataBuffer[5] & 0b00001100 >> 2));
    crsf.PackedRCdataOut.ch3 = UINT10_to_CRSF((Radio.RXdataBuffer[4] << 2) + (Radio.RXdataBuffer[5] & 0b00000011 >> 0));
}

void ICACHE_RAM_ATTR UnpackSwitchData()
{

    crsf.PackedRCdataOut.ch4 = SWITCH3b_to_CRSF((uint16_t)(Radio.RXdataBuffer[1] & 0b11100000) >> 5); //unpack the byte structure, each switch is stored as a possible 8 states (3 bits). we shift by 2 to translate it into the 0....1024 range like the other channel data.
    crsf.PackedRCdataOut.ch5 = SWITCH3b_to_CRSF((uint16_t)(Radio.RXdataBuffer[1] & 0b00011100) >> 2);
    crsf.PackedRCdataOut.ch6 = SWITCH3b_to_CRSF((uint16_t)((Radio.RXdataBuffer[1] & 0b00000011) << 1) + ((Radio.RXdataBuffer[2] & 0b10000000) >> 7));
    crsf.PackedRCdataOut.ch7 = SWITCH3b_to_CRSF((uint16_t)((Radio.RXdataBuffer[2] & 0b01110000) >> 4));
}

void ICACHE_RAM_ATTR ProcessRFPacket()
{
    //Serial.println("got pkt");
    uint8_t calculatedCRC = CalcCRC(Radio.RXdataBuffer, 7) + CRCCaesarCipher;
    uint8_t inCRC = Radio.RXdataBuffer[7];
    uint8_t type = Radio.RXdataBuffer[0] & 0b11;
    uint8_t packetAddr = (Radio.RXdataBuffer[0] & 0b11111100) >> 2;
    //Serial.print(NonceRXlocal);
    //Serial.print(" ");
    // Serial.print(inCRC);
    // Serial.print(" = ");
    // Serial.println(calculatedCRC);
    if (inCRC == calculatedCRC)
    {
        if (packetAddr == DeviceAddr)
        {
            packetCounter++;
            addPacketToLQ();
            //Serial.println(linkQuality);

            LastValidPacket = millis();

            HWtimerError = micros() - HWtimerGetlastCallbackMicros();

            HWtimerError90 = micros() - HWtimerGetlastCallbackMicros90();

            // uint32_t HWtimerInterval = HWtimerGetIntervalMicros();
            Offset = SimpleLowPass(HWtimerError - (ExpressLRS_currAirRate.interval / 2) + 0); //crude 'locking function' to lock hardware timer to transmitter, seems to work well enough
            HWtimerPhaseShift(Offset / 2);
            // Serial.println(Offset);

            if (type == 0b00) //std 4 channel switch data
            {
                UnpackChannelData_11bit();
                crsf.sendRCFrameToFC();
            }

            if (type == 0b01)
            {
                //Serial.println("Switch Packet");
                if ((Radio.RXdataBuffer[3] == Radio.RXdataBuffer[1]) && (Radio.RXdataBuffer[4] == Radio.RXdataBuffer[2])) // extra layer of protection incase the crc and addr headers fail us.
                {
                    GotConnection();
                    UnpackSwitchData();
                    NonceRXlocal = Radio.RXdataBuffer[5];
                    FHSSsetCurrIndex(Radio.RXdataBuffer[6]);
                    crsf.sendRCFrameToFC();
                }
            }

            if (type == 0b11)
            { //telemetry packet from master
                // not implimented yet
            }

            if (type == 0b10)
            {
                if (Radio.RXdataBuffer[4] == TxBaseMac[3] && Radio.RXdataBuffer[5] == TxBaseMac[4] && Radio.RXdataBuffer[6] == TxBaseMac[5])
                {
                    { //sync packet from master
                        Serial.println("Sync Packet");

                        FHSSsetCurrIndex(Radio.RXdataBuffer[1]);

                        //NonceRXlocal = (Radio.RXdataBuffer[2] & 0b11110000) >> 4;
                        NonceRXlocal = Radio.RXdataBuffer[3];

                        GotConnection();

                        if (ExpressLRS_currAirRate.enum_rate == !(expresslrs_RFrates_e)(Radio.RXdataBuffer[2] & 0b00001111))
                        {
                            Serial.println("update rate");
                            switch (Radio.RXdataBuffer[3])
                            {
                            case 0:
                                SetRFLinkRate(RF_RATE_200HZ);
                                ExpressLRS_currAirRate = RF_RATE_200HZ;
                                break;
                            case 1:
                                SetRFLinkRate(RF_RATE_100HZ);
                                ExpressLRS_currAirRate = RF_RATE_100HZ;
                                break;
                            case 2:
                                SetRFLinkRate(RF_RATE_50HZ);
                                ExpressLRS_currAirRate = RF_RATE_50HZ;
                                break;
                            default:
                                break;
                            }
                        }

                        //Serial.println()
                    }
                }
            }
        }
        else
        {
            Serial.println("wrong address");
        }

        getRFlinkInfo(); // run if CRC is valid
    }
    else
    {
        Serial.println("crc failed");
        //Serial.print(calculatedCRC);
        //Serial.print("-");
        //Serial.println(inCRC);
        CRCerrorCounter++;
    }
}

void beginWebsever()
{
#ifdef PLATFORM_STM32
#else
    Radio.StopContRX();
    StopHWtimer();

    BeginWebUpdate();
    webUpdateMode = true;
#endif
}

void ICACHE_RAM_ATTR sampleButton()
{

    // bool buttonValue = digitalRead(GPIO_PIN_BUTTON);

    // if (buttonValue == false && buttonPrevValue == true)
    // { //falling edge
    //     buttonLastPressed = millis();
    //     buttonDown = true;
    //     Serial.println("Manual Start");
    //     Radio.SetFrequency(GetInitialFreq());
    //     Radio.StartContRX();
    // }

    // if (buttonValue == true && buttonPrevValue == false) //rising edge
    // {
    //     buttonDown = false;
    // }

    // if ((millis() > buttonLastPressed + webUpdatePressInterval) && buttonDown)
    // {
    //     if (!webUpdateMode)
    //     {
    //         beginWebsever();
    //     }
    // }

    // if ((millis() > buttonLastPressed + buttonResetInterval) && buttonDown)
    // {
    //     ESP.restart();
    // }

    // buttonPrevValue = buttonValue;
}

void ICACHE_RAM_ATTR SetRFLinkRate(expresslrs_mod_settings_s mode) // Set speed of RF link (hz)
{
    Radio.StopContRX();
    Radio.Config(mode.bw, mode.sf, mode.cr, Radio.currFreq, Radio._syncWord);
    ExpressLRS_currAirRate = mode;
    HWtimerUpdateInterval(mode.interval);
    Radio.RXnb();
}

void setup()
{
#ifdef PLATFORM_STM32
    Serial.setTx(GPIO_PIN_RCSIGNAL_TX);
    Serial.setRx(GPIO_PIN_RCSIGNAL_RX);
    Serial.begin(420000);
    //Serial.begin(115200);
    crsf.InitSerial();
#endif

#ifdef PLATFORM_ESP8266
    Serial.begin(420000);
    //Serial.begin(115200);
#endif
    Serial.println("Module Booting...");
    pinMode(GPIO_PIN_LED, OUTPUT);
#ifdef PLATFORM_STM32
    pinMode(GPIO_PIN_LED_GEEN, OUTPUT);
    digitalWrite(GPIO_PIN_LED_GEEN, HIGH); // Turn on extra LED... because we can.
#endif
    pinMode(GPIO_PIN_BUTTON, INPUT);

    // delay(200);
    // digitalWrite(GPIO_PIN_LED, HIGH);
    // delay(200);
    // digitalWrite(GPIO_PIN_LED, LOW);
    // delay(200);
    // digitalWrite(GPIO_PIN_LED, HIGH);
    // delay(200);
    // digitalWrite(GPIO_PIN_LED, LOW);

    FHSSrandomiseFHSSsequence();

#ifdef Regulatory_Domain_AU_915
    Serial.println("Setting 915MHz Mode");
    Radio.RFmodule = RFMOD_SX1276; //define radio module here
#elif defined Regulatory_Domain_AU_433
    Serial.println("Setting 433MHz Mode");
    Radio.RFmodule = RFMOD_SX1278; //define radio module here
#endif

    Radio.SetFrequency(GetInitialFreq()); //set frequency first or an error will occur!!!

    Radio.Begin();

    Radio.SetOutputPower(0b1111);

    Radio.RXdoneCallback1 = &ProcessRFPacket;

    Radio.TXdoneCallback1 = &Radio.StartContRX;

    crsf.Begin();

    HWtimerSetCallback(&Test);
    HWtimerSetCallback90(&Test90);
    InitHarwareTimer();
    SetRFLinkRate(RF_RATE_200HZ);
    Serial.println(GetInitialFreq());
}

void loop()
{

#ifdef Auto_WiFi_On_Boot
    if (LostConnection && !webUpdateMode && millis() > 10000 && millis() < 11000)
    {
        beginWebsever();
    }
#endif

    if (millis() > (RFmodeLastCycled + RFmodeCycleInterval))
    {
        if (LostConnection && !webUpdateMode)

        {
            //StopHWtimer();
            Beta = 1;
            switch (scanIndex)
            {
            case 1:
                SetRFLinkRate(RF_RATE_200HZ);
                Radio.SetFrequency(GetInitialFreq());
                Serial.println("200 Hz");
                //delay(1000);
                break;
            case 2:
                SetRFLinkRate(RF_RATE_100HZ);
                Radio.SetFrequency(GetInitialFreq());
                Serial.println("100 Hz");
                //delay(1000);
                break;
            case 3:
                SetRFLinkRate(RF_RATE_50HZ);
                Radio.SetFrequency(GetInitialFreq());
                Serial.println("50 Hz");
                //delay(1000);
                break;
            default:
                break;
            }

            digitalWrite(GPIO_PIN_LED, LED);
            LED = !LED;

            if (scanIndex > 3)
            {
                scanIndex = 1;
            }
            else
            {

                scanIndex++;
            }
        }

        if (linkQuality < 10)
        {
            if (!LostConnection)
            {
                LostConnection = true;
                digitalWrite(GPIO_PIN_LED, 0);
            }
        }

        RFmodeLastCycled = millis();
    }

    if (millis() > (LastValidPacket + LostConnectionDelay))
    {
        if (!LostConnection)
        {
            LostConnection = true;
            digitalWrite(GPIO_PIN_LED, 0);
        }
    }
    else
    {
        digitalWrite(GPIO_PIN_LED, 1);
    }

    // if (millis() > LastSerialDebugPrint + SerialDebugPrintInterval)
    // { // add stuff here for debug print
    //     LastSerialDebugPrint = millis();
    //     Serial.println(linkQuality);
    //     if (LostConnection)
    //     {
    //         Serial.println("-");
    //     }
    //     else
    //     {
    //         Serial.println("+");
    //     }

    //     Serial.print(MeasuredHWtimerInterval);
    //     Serial.print(" ");
    //     Serial.print(Offset);
    //     Serial.print(" ");
    //     Serial.print(HWtimerError);

    //     Serial.print("----");

    //     Serial.print(Offset90);
    //     Serial.print(" ");
    //     Serial.print(HWtimerError90);
    //     Serial.print("----");
    //     Serial.println(packetCounter);
    // }

    // if (millis() > (PacketRateLastChecked + PacketRateInterval)) //just some debug data
    // {
    //     //     float targetFrameRate;

    //     //     if (ExpressLRS_currAirRate.TLMinterval != 0)
    //     //     {
    //     //         targetFrameRate = ExpressLRS_currAirRate.rate - ((ExpressLRS_currAirRate.rate) * (1.0 / ExpressLRS_currAirRate.TLMinterval));
    //     //     }
    //     //     else
    //     //     {
    //     //         targetFrameRate = ExpressLRS_currAirRate.rate;
    //     //     }

    //     PacketRateLastChecked = millis();
    //     //     PacketRate = (float)packetCounter / (float)(PacketRateInterval);
    //     //     linkQuality = int(((float)PacketRate / (float)targetFrameRate) * 100000.0);
    //     //     if(linkQuality > 99) linkQuality = 99;

    //     //     CRCerrorRate = (((float)CRCerrorCounter / (float)(PacketRateInterval)) * 100);

    //     //     CRCerrorCounter = 0;
    //     //     packetCounter = 0;

    //     //     //Serial.println(CRCerrorRate);
    //     // }
    //     Serial.print(MeasuredHWtimerInterval);
    //     Serial.print(" ");
    //     Serial.print(Offset);
    //     Serial.print(" ");
    //     Serial.print(HWtimerError);

    //     Serial.print("----");

    //     Serial.print(Offset90);
    //     Serial.print(" ");
    //     Serial.print(HWtimerError90);
    //     Serial.print("----");

    //Serial.println(linkQuality);
    //     //Serial.println(packetCounter);
    // }

    // Serial.print(MeasuredHWtimerInterval);
    // Serial.print(" ");
    // Serial.print(Offset);
    // Serial.print(" ");
    // Serial.print(HWtimerError);

    // Serial.print("----");

    // Serial.print(Offset90);
    // Serial.print(" ");
    // Serial.print(HWtimerError90);
    // Serial.print("----");
    // Serial.println(packetCounter);
    // delay(200);

    if (millis() > (buttonLastSampled + buttonSampleInterval))
    {
        sampleButton();
        buttonLastSampled = millis();
    }

    //yield();
#ifdef PLATFORM_STM32
#else
    if (webUpdateMode)
    {
        HandleWebUpdate();
        if (millis() > webUpdateLedFlashInterval + webUpdateLedFlashIntervalLast)
        {
            digitalWrite(GPIO_PIN_LED, LED);
            LED = !LED;
            webUpdateLedFlashIntervalLast = millis();
        }
    }
#endif
}