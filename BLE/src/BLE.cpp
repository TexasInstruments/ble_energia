
#include <string.h>

#include <ti/sysbios/BIOS.h>
#include <ti/sysbios/knl/Clock.h>
#include <ti/sysbios/knl/Event.h>
#include <ti/drivers/UART.h>

#include "ti/sap/sap.h"
#include "ti/sap/snp.h"
#include "ti/sap/snp_rpc.h"
/*
 * This is strictly to force the build system to compile npi into
 * its own object files for the linker to link. It isn't used here.
 */
#include "ti/npi/npi_task.h"

#include <BLE.h>
#include "BLEBoard.h"
#include "BLELog.h"
#include "BLESerial.h"
#include "BLEServiceList.h"
#include "BLEServices.h"
#include "Debug.h"

/*
 * Event_pend timeout set in units of ticks. Tick period is microseconds,
 * so this evaluates to 1 second.
 */
#define AP_EVENT_PEND_TIMEOUT                (1000000/Clock_tickPeriod)

// Event ID's are integers
#define AP_NONE                              Event_Id_NONE   // No Event
#define AP_EVT_PUI                           Event_Id_00     // Power-Up Indication
#define AP_EVT_ADV_ENB                       Event_Id_01     // Advertisement Enabled
#define AP_EVT_ADV_END                       Event_Id_02     // Advertisement Ended
#define AP_EVT_ADV_DATA_RSP                  Event_Id_03     // Advertisement Data Set Response
#define AP_EVT_CONN_EST                      Event_Id_04     // Connection Established
#define AP_EVT_CONN_TERM                     Event_Id_05     // Connection Terminated
#define AP_EVT_HCI_RSP                       Event_Id_06     // HCI Command Response
#define AP_EVT_TEST_RSP                      Event_Id_07     // Test Command Response
#define AP_EVT_CONN_PARAMS_UPDATED           Event_Id_08     // Connection Parameters Updated
#define AP_EVT_CONN_PARAMS_CNF               Event_Id_09     // Connection Parameters Request Confirmation
#define AP_EVT_NOTIF_IND_RSP                 Event_Id_10     // Notification/Indication Response
#define AP_EVT_HANDLE_AUTH_EVT               Event_Id_11     // Authentication Required Event
#define AP_EVT_AUTH_RSP                      Event_Id_12     // Set Authentication Data Response
#define AP_EVT_SECURITY_STATE                Event_Id_13     // Security State Changed
#define AP_EVT_SECURITY_PARAM_RSP            Event_Id_14     // Set Security Param Response
#define AP_EVT_WHITE_LIST_RSP                Event_Id_15     // Set White List Policy Response
#define AP_EVT_NUM_CMP_BTN                   Event_Id_16     // Numeric Comparison Button Press
#define AP_EVT_COPIED_ASYNC_DATA             Event_Id_30     // Copied Data From asyncRspData
#define AP_ERROR                             Event_Id_31     // Error

// From bcomdef.h in the BLE SDK
#define B_ADDR_LEN 6


/* Macro abuse for setting up many pins for debug */
#define DEBUG_FXN(pin, pinNum) \
int isOutput##pin = 0; \
void ping##pin(void) \
{ \
 if (!isOutput##pin) \
 { \
   pinMode(pinNum, OUTPUT); \
   isOutput##pin = 1; \
 } \
 ping(pinNum); \
}
static void ping(int pin)
{
 digitalWrite(pin, HIGH);
 digitalWrite(pin, LOW);
}
DEBUG_PINS_LIST
#undef DEBUG_FXN
/* End abuse */

Event_Handle apEvent = NULL;
snp_msg_t *asyncRspData = NULL;
snpEventParam_t eventHandlerData;

/* Here instead of private class members so static callbacks can write them */
uint16_t _connHandle = -1;
bool connected = false;
bool advertising = false;

BLE ble = BLE();

// GAP - Advertisement data (max size = 31 bytes, though this is
// best kept short to conserve power while advertisting)
static uint8_t defNotConnAD[] =
{
  0x02,   // length of this data
  SAP_GAP_ADTYPE_FLAGS,
  SAP_GAP_ADTYPE_FLAGS_GENERAL | SAP_GAP_ADTYPE_FLAGS_BREDR_NOT_SUPPORTED,

  // Manufacturer specific advertising data
  0x06,
  0xFF, //GAP_ADTYPE_MANUFACTURER_SPECIFIC,
  LO_UINT16(TI_COMPANY_ID),
  HI_UINT16(TI_COMPANY_ID),
  TI_ST_DEVICE_ID,
  TI_ST_KEY_DATA_ID,
  0x00                                    // Key state
};

/*
 * The SNP will automatically use the non-connectable advertisement data
 * if the connectable advertisement data is not set.
 */
static uint8_t defConnAD[0] = {};

static uint8_t defScanRspData[] = {
  // complete name
  0xc,// length of this data
  SAP_GAP_ADTYPE_LOCAL_NAME_COMPLETE,
  'E', 'n', 'e', 'r', 'g', 'i', 'a', ' ',
  'B', 'L', 'E',

  // connection interval range
  0x05,   // length of this data
  0x12, //GAP_ADTYPE_SLAVE_CONN_INTERVAL_RANGE,
  LO_UINT16( BLE_DEF_DESIRED_MIN_CONN_INT ),
  HI_UINT16( BLE_DEF_DESIRED_MIN_CONN_INT ),
  LO_UINT16( BLE_DEF_DESIRED_MAX_CONN_INT ),
  HI_UINT16( BLE_DEF_DESIRED_MAX_CONN_INT ),

  // Tx power level
  0x02,   // length of this data
  0x0A, //GAP_ADTYPE_POWER_LEVEL,
  0       // 0dBm
};

static uint8_t *defADArr[] =
{
  defNotConnAD,
  defConnAD,
  defScanRspData
};

static size_t defADSizes[] =
{
  sizeof(defNotConnAD),
  sizeof(defConnAD),
  sizeof(defScanRspData)
};

static uint8_t aDIdxToType[] =
{
  BLE_ADV_DATA_NOTCONN,
  BLE_ADV_DATA_CONN,
  BLE_ADV_DATA_SCANRSP
};

static uint8_t advertIndex(uint8_t advertType);
static void AP_asyncCB(uint8_t cmd1, void *pParams);
static void processSNPEventCB(uint16_t event, snpEventParam_t *param);
static void numCmpInterrupt1(void);
static void numCmpInterrupt2(void);
static bool apEventPend(uint32_t event);
static inline void apPostError(uint8_t status, const char errMsg[]);
static bool isError(uint8_t status);

BLE::BLE(byte portType)
{
  _portType = portType;
  for (uint8_t idx = 0; idx < MAX_ADVERT_IDX; idx++) {advertDataArr[idx] = NULL;}
  displayStringFxn = NULL;
  displayUIntFxn = NULL;
  resetPublicMembers();
}

int BLE::begin(void)
{
  /* Do board specific initializations */
  initBoard();

  /* AP_init() in simple_ap.c */
  apEvent = Event_create(NULL, NULL);
  logSetMainTask(Task_self());

  /*
   * Use this to do something at the application level on a write or
   * config change. In each SAP service struct, we set read, write, and
   * config change callbacks. In simple_ap, the function below
   * registers its parameters as functions called by the callbacks.
   * In other words, this isn't actually necessary for SAPlib or BLE.
   * It's just included for now for completeless with simple_ap.
   */
  // SimpleProfile_RegisterAppCB(AP_SPWriteCB, AP_SPcccdCB);
  /* End AP_init() */

  SAP_Params sapParams;
  SAP_initParams(_portType, &sapParams);
  sapParams.port.remote.boardID = BLE_UART_ID;
  sapParams.port.remote.mrdyPinID = BLE_Board_MRDY;
  sapParams.port.remote.srdyPinID = BLE_Board_SRDY;
  logRPC("Opening SAP");
  if (isError(SAP_open(&sapParams)))
  {
    SAP_close();
    return BLE_CHECK_ERROR;
  }

  /*
   * Register callback to receive asynchronous requests from the NP.
   * This must be called before using any other calls to SAP, except
   * those above. This function may be called multiple times to
   * register multiple callbacks. Runs in NPI task.
   */
  if (isError(SAP_setAsyncCB(AP_asyncCB)))
  {
    SAP_close();
    return BLE_CHECK_ERROR;
  }

  /*
   * Register async SNP event handler. Includes connection establisment,
   * termination, advertisement enabling, security events.
   */
  if (isError(SAP_registerEventCB(processSNPEventCB, 0xFFFF)))
  {
    SAP_close();
    return BLE_CHECK_ERROR;
  }

  /*
   * Give the NP time to send a power up indicator, if we receive one then
   * we can assume the NP has just started and we don't need to send a reset
   * otherwise, we can assume the NP was running previously and needs to be
   * reset to a known state
   */
  if (!apEventPend(AP_EVT_PUI)) {
    // Assuming that at SAP start up that SNP is already running
    logRPC("Reseting SNP");
    if (isError(SAP_reset()))
    {
      SAP_close();
      return BLE_CHECK_ERROR;
    }
    if (!apEventPend(AP_EVT_PUI))
    {
      return BLE_CHECK_ERROR;
    }
  }

  return BLE_SUCCESS;
}

void BLE::end(void)
{
  /* Reset private members of BLE.h */
  for (uint8_t idx = 0; idx < MAX_ADVERT_IDX; idx++) {advertDataArr[idx] = NULL;}

  /* Reset public members of BLE.h */
  resetPublicMembers();

  BLE_clearServices();
  flush();
  logReset();
  Event_delete(&apEvent);
  free(asyncRspData);
  asyncRspData = NULL;
  memset(&eventHandlerData, 0, sizeof(eventHandlerData));
  _connHandle = -1;
  connected = false;
  advertising = false;
  SAP_close();
}

int BLE::resetPublicMembers(void)
{
  error = BLE_SUCCESS;
  opcode = 0;
  memset(&usedConnParams, 0, sizeof(usedConnParams));
  memset(&bleAddr, 0, sizeof(bleAddr));
  authKey = 0;
  mtu = 20;
  displayStringFxn = NULL;
  displayUIntFxn = NULL;
  return BLE_SUCCESS;
}

int BLE::terminateConn(void)
{
  logRPC("Terminate connection");
  if ((!connected && isError(BLE_NOT_CONNECTED)) ||
      isError(SAP_setParam(SAP_PARAM_CONN, SAP_CONN_STATE,
                           sizeof(_connHandle), (uint8_t *) &_connHandle)) ||
      !apEventPend(AP_EVT_CONN_TERM))
  {
    return BLE_CHECK_ERROR;
  }
  return BLE_SUCCESS;
}

bool BLE::isConnected(void)
{
  return connected;
}

bool BLE::isAdvertising(void)
{
  return advertising;
}

int BLE::addService(BLE_Service *bleService)
{
  if (isError(BLE_registerService(bleService)))
  {
    return BLE_CHECK_ERROR;
  }
  return BLE_SUCCESS;
}

static uint8_t advertIndex(uint8_t advertType)
{
  switch (advertType)
  {
    case BLE_ADV_DATA_NOTCONN:
      return 0;
    case BLE_ADV_DATA_CONN:
      return 1;
    case BLE_ADV_DATA_SCANRSP:
      return 2;
  }
  return BLE_INVALID_PARAMETERS;
}

uint8_t BLE::advertDataInit(void)
{
  for (uint8_t idx = 0; idx < MAX_ADVERT_IDX; idx++)
  {
    if (advertDataArr[idx] == NULL && defADArr[idx] != NULL && defADSizes[idx])
    {
      if (isError(setAdvertData(aDIdxToType[idx], defADSizes[idx],
                                defADArr[idx])))
      {
        return BLE_CHECK_ERROR;
      }
    }
  }
  return BLE_SUCCESS;
}

int BLE::startAdvert(BLE_Advert_Settings *advertSettings)
{
  logRPC("Start adv");
  if ((advertising && isError(BLE_ALREADY_ADVERTISING)) ||
      isError(advertDataInit()))
  {
    return BLE_CHECK_ERROR;
  }

  uint16_t reqSize;
  uint8_t *pData;
  /* Declare these outside the if statements so they're in-scope
     for SAP_setParam. */
  uint8_t enableAdv = SAP_ADV_STATE_ENABLE;
  snpStartAdvReq_t lReq;
  if (advertSettings == NULL)
  {
    reqSize = 1;
    pData = &enableAdv;
  }
  else
  {
    lReq.type = advertSettings->advertMode;
    lReq.timeout = advertSettings->timeout;
    lReq.interval = advertSettings->interval;
    lReq.behavior = advertSettings->connectedBehavior;
    reqSize = (uint16_t) sizeof(lReq);
    pData = (uint8_t *) &lReq;
  }
  if (isError(SAP_setParam(SAP_PARAM_ADV, SAP_ADV_STATE, reqSize, pData)) ||
      !apEventPend(AP_EVT_ADV_ENB))
  {
    return BLE_CHECK_ERROR;
  }
  return BLE_SUCCESS;
}

int BLE::stopAdvert(void)
{
  uint8_t disableAdv = SAP_ADV_STATE_DISABLE;
  logRPC("End adv");
  if ((!advertising && isError(BLE_NOT_ADVERTISING)) ||
      isError(SAP_setParam(SAP_PARAM_ADV, SAP_ADV_STATE, 1, &disableAdv)) ||
      !apEventPend(AP_EVT_ADV_END))
  {
    return BLE_CHECK_ERROR;
  }
  return BLE_SUCCESS;
}

int BLE::setAdvertData(uint8_t advertType, uint8_t len, uint8_t *advertData)
{
  logRPC("Set adv data");
  logParam("Type", advertType);
  if (isError(SAP_setParam(SAP_PARAM_ADV, advertType, len, advertData)) ||
      !apEventPend(AP_EVT_ADV_DATA_RSP))
  {
    return BLE_CHECK_ERROR;
  }
  // advertType validated by SAP_setParam
  uint8_t idx = advertIndex(advertType);
  if (advertDataArr[idx] && advertDataArr[idx] != defADArr[idx])
  {
    free(advertDataArr[idx]);
  }
  advertDataArr[idx] = advertData;
  return BLE_SUCCESS;
}

/*
 * Uses the default scan response data defScanRspData. The first
 * byte is one plus the length of the name. The second should be
 * SAP_GAP_ADTYPE_LOCAL_NAME_COMPLETE, and the third and so on are the
 * characters of the name.
 */
int BLE::setAdvertName(uint8_t advertNameLen, const char advertName[])
{
  uint8_t newSize = sizeof(defScanRspData) - defScanRspData[0]
                  + 1 + advertNameLen;
  uint8_t *newData = (uint8_t *) malloc(newSize * sizeof(*newData));
  newData[0] = 1 + advertNameLen;
  newData[1] = defScanRspData[1];
  strcpy((char *) &newData[2], advertName);
  uint8_t *destAfterStr = newData + 2 + advertNameLen;
  uint8_t *srcAfterStr = defScanRspData + 1 + defScanRspData[0];
  uint8_t afterStrLen = sizeof(defScanRspData) - 1 - defScanRspData[0];
  memcpy(destAfterStr, srcAfterStr, afterStrLen);
  logRPC("Set adv name");
  logParam(advertName);
  return setAdvertData(BLE_ADV_DATA_SCANRSP, newSize, newData);
}

int BLE::setAdvertName(const char advertName[])
{
  return setAdvertName(strlen(advertName), advertName);
}

int BLE::setAdvertName(String *advertName)
{
  uint8_t len = (*advertName).length();
  return setAdvertName(len, (*advertName).c_str());
}

int BLE::setGattParam(uint8_t serviceId, uint8_t charId,
                      uint16_t len, uint8_t *pData)
{
  logRPC("Set GATT param");
  return SAP_setServiceParam(serviceId, charId, len, pData);
}

int BLE::getGattParam(uint8_t serviceId, uint8_t charId,
                      uint16_t *len, uint8_t *pData)
{
  logRPC("Get GATT param");
  return SAP_getServiceParam(serviceId, charId, len, pData);
}

int BLE::setGapParam(uint16_t paramId, uint16_t value)
{
  logRPC("Set GAP param");
  logParam("Param ID", paramId);
  logParam("Value", value);
  return SAP_setParam(SAP_PARAM_GAP, paramId,
                      sizeof(value), (uint8_t *) &value);
}

int BLE::getGapParam(uint16_t paramId, uint16_t *value)
{
  logRPC("Get GAP param");
  logParam("Param ID", paramId);
  return SAP_setParam(SAP_PARAM_GAP, paramId,
                      sizeof(*value), (uint8_t *) value);
}

uint8_t *BLE::hciCommand(uint16_t opcode, uint16_t len, uint8_t *pData)
{
  logRPC("HCI cmd");
  logParam("Opcode", opcode);
  if (isError(SAP_getParam(SAP_PARAM_HCI, opcode, len, pData)) ||
      !apEventPend(AP_EVT_HCI_RSP))
  {
    return NULL;
  }
  snpHciCmdRsp_t hciCmdRsp;
  memcpy(&hciCmdRsp, asyncRspData, sizeof(hciCmdRsp));
  Event_post(apEvent, AP_EVT_COPIED_ASYNC_DATA);
  return hciCmdRsp.pData; // TODO: does this get deallocated in NPI task?
}

/*
 * Must be currently in a connection.
 */
int BLE::setConnParams(BLE_Conn_Params_Update_Req *connParams)
{
  logRPC("Conn params req");
  logParam("intervalMin", connParams->intervalMin);
  logParam("intervalMax", connParams->intervalMax);
  logParam("slaveLatency", connParams->slaveLatency);
  logParam("supervisionTimeout", connParams->supervisionTimeout);
  if ((!connected && isError(BLE_NOT_CONNECTED)) ||
      isError(SAP_setParam(SAP_PARAM_CONN, SAP_CONN_PARAM,
                           sizeof(*connParams), (uint8_t *) connParams)) ||
      !apEventPend(AP_EVT_CONN_PARAMS_CNF))
  {
    return BLE_CHECK_ERROR;
  }
  return BLE_SUCCESS;
}

int BLE::setSingleConnParam(size_t offset, uint16_t value)
{
  BLE_Conn_Params_Update_Req paramsReq;
  paramsReq.connHandle         = _connHandle;
  paramsReq.intervalMin        = SNP_CONN_INT_MIN;
  paramsReq.intervalMax        = SNP_CONN_INT_MAX;
  paramsReq.slaveLatency       = usedConnParams.slaveLatency;
  paramsReq.supervisionTimeout = usedConnParams.supervisionTimeout;
  *(uint16_t *)(((char *) &paramsReq) + offset) = value;
  return setConnParams(&paramsReq);
}

int BLE::setMinConnInt(uint16_t intervalMin)
{
  return setSingleConnParam(offsetof(BLE_Conn_Params_Update_Req, intervalMin),
                            intervalMin);
}

int BLE::setMaxConnInt(uint16_t intervalMax)
{
  return setSingleConnParam(offsetof(BLE_Conn_Params_Update_Req, intervalMax),
                            intervalMax);
}

int BLE::setRespLatency(uint16_t slaveLatency)
{
  return setSingleConnParam(offsetof(BLE_Conn_Params_Update_Req, slaveLatency),
                            slaveLatency);
}

int BLE::setBleTimeout(uint16_t supervisionTimeout)
{
  return setSingleConnParam(offsetof(BLE_Conn_Params_Update_Req, supervisionTimeout),
                            supervisionTimeout);
}

int BLE::apCharWriteValue(BLE_Char *bleChar, void *pData,
                          size_t size, bool isBigEnd=true)
{
  logChar("App writing");
  BLE_charWriteValue(bleChar, pData, size, isBigEnd);
  return writeNotifInd(bleChar);
}

uint8_t BLE::writeNotifInd(BLE_Char *bleChar)
{
  if (bleChar->_CCCD)
  {
    snpNotifIndReq_t localReq;
    localReq.connHandle = _connHandle;
    localReq.attrHandle = bleChar->_handle;
    localReq.authenticate = 0;
    if (bleChar->_CCCD & SNP_GATT_CLIENT_CFG_NOTIFY)
    {
      localReq.type = SNP_SEND_NOTIFICATION;
      logRPC("Sending notif");
    }
    else if (bleChar->_CCCD & SNP_GATT_CLIENT_CFG_INDICATE)
    {
      localReq.type = SNP_SEND_INDICATION;
      logRPC("Sending ind");
    }
    logParam("Total bytes", bleChar->_valueLen);
    uint16_t sent = 0;
    /* Send at least one notification, in case data is 0 length. */
    do
    {
      /* Send at most ble.mtu per packet. */
      uint16_t size = MIN(bleChar->_valueLen - sent, mtu);
      localReq.pData = ((uint8_t *) bleChar->_value) + sent;
      logParam("Sending", size);
      if (isError(SNP_RPC_sendNotifInd(&localReq, size)))
      {
        return BLE_CHECK_ERROR;
      }
      // Only pend for confirmation of indication
      if ((bleChar->_CCCD & SNP_GATT_CLIENT_CFG_INDICATE) &&
               !apEventPend(AP_EVT_NOTIF_IND_RSP))
      {
        return BLE_CHECK_ERROR;
      }
      sent += size;
    } while (sent < bleChar->_valueLen);
  }
  return BLE_SUCCESS;
}

int BLE::writeValue(BLE_Char *bleChar, bool value)
{
  return apCharWriteValue(bleChar, (uint8_t *) &value, sizeof(value), false);
}

int BLE::writeValue(BLE_Char *bleChar, char value)
{
  return apCharWriteValue(bleChar, (uint8_t *) &value, sizeof(value), false);
}

int BLE::writeValue(BLE_Char *bleChar, unsigned char value)
{
  return apCharWriteValue(bleChar, (uint8_t *) &value, sizeof(value), false);
}

int BLE::writeValue(BLE_Char *bleChar, int value)
{
  return apCharWriteValue(bleChar, (uint8_t *) &value, sizeof(value), false);
}

int BLE::writeValue(BLE_Char *bleChar, unsigned int value)
{
  return apCharWriteValue(bleChar, (uint8_t *) &value, sizeof(value), false);
}

int BLE::writeValue(BLE_Char *bleChar, long value)
{
  return apCharWriteValue(bleChar, (uint8_t *) &value, sizeof(value), false);
}

int BLE::writeValue(BLE_Char *bleChar, unsigned long value)
{
  return apCharWriteValue(bleChar, (uint8_t *) &value, sizeof(value), false);
}

int BLE::writeValue(BLE_Char *bleChar, float value)
{
  return apCharWriteValue(bleChar, (uint8_t *) &value, sizeof(value), true);
}

int BLE::writeValue(BLE_Char *bleChar, double value)
{
  return apCharWriteValue(bleChar, (uint8_t *) &value, sizeof(value), true);
}

int BLE::writeValue(BLE_Char *bleChar, const uint8_t buf[], int len)
{
  return apCharWriteValue(bleChar, (uint8_t *) buf, (len)*sizeof(*buf), true);
}

/*
 * Use buffer of size len+1 so the null-termination is stored. This way the
 * stored strings match the functionality of strcpy, which copies it.
 */
int BLE::writeValue(BLE_Char *bleChar, const char str[], int len)
{
  int ret = apCharWriteValue(bleChar, (uint8_t *) str, (len+1)*sizeof(*str), true);
  logParam("As string", str);
  return ret;
}

int BLE::writeValue(BLE_Char *bleChar, const char str[])
{
  return writeValue(bleChar, str, strlen(str));
}

int BLE::writeValue(BLE_Char *bleChar, String *str)
{
  int len = (*str).length();
  return writeValue(bleChar, (*str).c_str(), len);
}

uint8_t BLE::readValueValidateSize(BLE_Char *bleChar, size_t size)
{
  logChar("App reading");
  logParam("Handle", bleChar->_handle);
  if (bleChar->_valueLen != size)
  {
    logParam("Invalid size");
    logParam("Have", bleChar->_valueLen);
    logParam("Want", size);
    error = BLE_UNDEFINED_VALUE;
    return BLE_CHECK_ERROR;
  }
  logParam("Size in bytes", size);
  logParam("Value", (const uint8_t *) bleChar->_value, size, bleChar->_isBigEnd);
  return BLE_SUCCESS;
}

bool BLE::readValue_bool(BLE_Char *bleChar)
{
  error = readValueValidateSize(bleChar, sizeof(bool));
  if (error == BLE_SUCCESS)
  {
    return *(bool *) bleChar->_value;
  }
  return 0;
}

char BLE::readValue_char(BLE_Char *bleChar)
{
  error = readValueValidateSize(bleChar, sizeof(char));
  if (error == BLE_SUCCESS)
  {
    return *(char *) bleChar->_value;
  }
  return 0;
}

unsigned char BLE::readValue_uchar(BLE_Char *bleChar)
{
  error = readValueValidateSize(bleChar, sizeof(unsigned char));
  if (error == BLE_SUCCESS)
  {
    return *(unsigned char *) bleChar->_value;
  }
  return 0;
}

int BLE::readValue_int(BLE_Char *bleChar)
{
  error = readValueValidateSize(bleChar, sizeof(int));
  if (error == BLE_SUCCESS)
  {
    return *(int *) bleChar->_value;
  }
  return 0;
}

unsigned int BLE::readValue_uint(BLE_Char *bleChar)
{
  error = readValueValidateSize(bleChar, sizeof(unsigned int));
  if (error == BLE_SUCCESS)
  {
    return *(unsigned int *) bleChar->_value;
  }
  return 0;
}

long BLE::readValue_long(BLE_Char *bleChar)
{
  error = readValueValidateSize(bleChar, sizeof(long));
  if (error == BLE_SUCCESS)
  {
    return *(long *) bleChar->_value;
  }
  return 0;
}

unsigned long BLE::readValue_ulong(BLE_Char *bleChar)
{
  error = readValueValidateSize(bleChar, sizeof(unsigned long));
  if (error == BLE_SUCCESS)
  {
    return *(unsigned long *) bleChar->_value;
  }
  return 0;
}

float BLE::readValue_float(BLE_Char *bleChar)
{
  error = readValueValidateSize(bleChar, sizeof(float));
  if (error == BLE_SUCCESS)
  {
    return *(float *) bleChar->_value;
  }
  return 0;
}

double BLE::readValue_double(BLE_Char *bleChar)
{
  error = readValueValidateSize(bleChar, sizeof(double));
  if (error == BLE_SUCCESS)
  {
    return *(double *) bleChar->_value;
  }
  return 0;
}

uint8_t* BLE::readValue_uint8_t(BLE_Char *bleChar, int *len)
{
  *len = bleChar->_valueLen;
  logChar("App reading");
  logParam("Handle", bleChar->_handle);
  logParam("Buffer length", *len);
  logParam("Buffer contents", (const uint8_t *) bleChar->_value, *len, true);
  return (uint8_t *) bleChar->_value;
}

char* BLE::readValue_charArr(BLE_Char *bleChar)
{
  int len = bleChar->_valueLen;
  logChar("App reading");
  logParam("Handle", bleChar->_handle);
  logParam("String length", len);
  /* Convert value to null-termiated string, if not already */
  if (((char *) bleChar->_value)[len-1] != '\0')
  {
    bleChar->_value = realloc(bleChar->_value, (len+1)*sizeof(char));
    ((char *) bleChar->_value)[len] = '\0';
  }
  logParam("As string", (const char *) bleChar->_value);
  return (char *) bleChar->_value;
}

/* Returns object by value instead of reference so the Energia user
   doesn't have to care about deallocating the object. */
String BLE::readValue_String(BLE_Char *bleChar)
{
  char *buf = readValue_charArr(bleChar);
  String str;
  if (buf)
  {
    str = String(buf);
  }
  else
  {
    str = String();
  }
  return str;
}

void BLE::setValueFormat(BLE_Char *bleChar, uint8_t valueFormat,
                         int8_t valueExponent)
{
  bleChar->_valueFormat = valueFormat;
  bleChar->_valueExponent = valueExponent;
}

int BLE::setSecurityParam(uint16_t paramId, uint16_t len, uint8_t *pData)
{
  logRPC("Set sec param");
  logParam("ParamId", paramId, len);
  if (isError(SAP_setParam(SAP_PARAM_SECURITY, paramId, len, pData)))
  {
    return BLE_CHECK_ERROR;
  }
  return BLE_SUCCESS;
}

int BLE::setPairingMode(uint8_t pairingMode)
{
  return setSecurityParam(SAP_SECURITY_BEHAVIOR, 1, &pairingMode);
}

int BLE::setIoCapabilities(uint8_t param)
{
  return setSecurityParam(SAP_SECURITY_IOCAPS, 1, &param);
}

int BLE::useBonding(bool param)
{
  return setSecurityParam(SAP_SECURITY_BONDING, 1, (uint8_t*) &param);
}

int BLE::eraseAllBonds(void)
{
  return setSecurityParam(SAP_ERASE_ALL_BONDS, 0, NULL);
}

int BLE::replaceLruBond(bool param)
{
  return setSecurityParam(SAP_ERASE_LRU_BOND, 1, (uint8_t*) &param);
}

int BLE::sendSecurityRequest(void)
{
  logRPC("Send sec req");
  return SAP_sendSecurityRequest();
}

int BLE::useWhiteListPolicy(uint8_t policy)
{
  logRPC("Use whitelist policy");
  logParam("policy", policy);
  if (isError(SAP_setParam(SAP_PARAM_WHITELIST, 0, 0, &policy)) ||
      !apEventPend(AP_EVT_WHITE_LIST_RSP))
  {
    return BLE_CHECK_ERROR;
  }
  return BLE_SUCCESS;
}

unsigned int BLE::getRand(void)
{
  return SAP_getRand();
}

/*
 * Rarely used and advanced-use calls so we won't provide a framework for this.
 */
void BLE::getRevision(BLE_Get_Revision_Rsp *getRevisionRsp)
{
  logRPC("Get revision");
  SAP_getRevision(getRevisionRsp);
}

void BLE::getStatus(BLE_Get_Status_Rsp *getStatusRsp)
{
  logRPC("Get status");
  SAP_getStatus(getStatusRsp);
}

int BLE::testCommand(BLE_Test_Command_Rsp *testRsp)
{
  logRPC("Test cmd");
  SAP_testCommand(); // void function
  if (!apEventPend(AP_EVT_TEST_RSP))
  {
    return BLE_CHECK_ERROR;
  }
  memcpy(testRsp, asyncRspData, sizeof(*testRsp));
  Event_post(apEvent, AP_EVT_COPIED_ASYNC_DATA);
  return BLE_SUCCESS;
}

int BLE::serial(void)
{
  if (isError(addService(&serialService)))
  {
    return BLE_CHECK_ERROR;
  }
  return BLE_SUCCESS;
}

int BLE::available(void)
{
  return BLESerial_available();
}

int BLE::read(void)
{
  return BLESerial_read();
}

int BLE::peek(void)
{
  return BLESerial_peek();
}

void BLE::flush(void)
{
  return BLESerial_flush();
}

size_t BLE::write(uint8_t c)
{
  if (writeValue(&txChar, c) == BLE_SUCCESS)
  {
    return 1;
  }
  return 0;
}

size_t BLE::write(const uint8_t buffer[], size_t size)
{
  if (writeValue(&txChar, buffer, size) == BLE_SUCCESS)
  {
    return size;
  }
  return 0;
}

int BLE::handleEvents(void)
{
  apLogLock = false;
  uint32_t events = AP_EVT_HANDLE_AUTH_EVT | AP_EVT_NUM_CMP_BTN;
  opcode = Event_pend(apEvent, AP_NONE, events, 1);
  int status = BLE_SUCCESS;
  if (opcode & AP_EVT_HANDLE_AUTH_EVT)
  {
    snpAuthenticationEvt_t *evt = (snpAuthenticationEvt_t *) &eventHandlerData;
    if (evt->numCmp)
    {
      handleNumCmp(evt);
    }
    else if (evt->display)
    {
      if (isError(handleAuthKey(evt)))
      {
        status = BLE_CHECK_ERROR;
      }
    }
  }
  if (opcode & AP_EVT_NUM_CMP_BTN)
  {
    detachInterrupt(PUSH1);
    detachInterrupt(PUSH2);
    logRPC("Send num cmp rsp");
    if (isError(SAP_setAuthenticationRsp(authKey)) ||
        !apEventPend(AP_EVT_AUTH_RSP))
    {
      status = BLE_CHECK_ERROR;
    }
  }
  apLogLock = true;
  return status;
}

int BLE::handleAuthKey(snpAuthenticationEvt_t *evt)
{
  authKey = getRand() % 1000000;
  if (displayStringFxn && displayUIntFxn)
  {
    displayStringFxn("Auth key:");
    displayUIntFxn(authKey);
    displayStringFxn("\n");
  }
  else if (Serial)
  {
    Serial.print("Auth key:");
    Serial.println(authKey);
  }
  if (evt->input)
  {
    logRPC("Send auth key");
    if (isError(SAP_setAuthenticationRsp(authKey)) ||
      !apEventPend(AP_EVT_AUTH_RSP))
    {
      return BLE_CHECK_ERROR;
    }
  }
  return BLE_SUCCESS;
}

void BLE::handleNumCmp(snpAuthenticationEvt_t *evt)
{
  if (displayStringFxn && displayUIntFxn)
  {
    displayStringFxn("Check if equal:");
    displayUIntFxn(evt->numCmp);
    displayStringFxn("\n");
  }
  else if (Serial)
  {
    Serial.print("Check if equal:");
    Serial.println(evt->numCmp);
  }
  if (evt->input)
  {
    if (displayStringFxn && displayUIntFxn)
    {
      displayStringFxn("Press button1 if equal, button2 if not.");
      displayStringFxn("\n");
    }
    else if (Serial)
    {
      Serial.println("Press button1 if equal, button2 if not.");
    }
    pinMode(PUSH1, INPUT_PULLUP);
    attachInterrupt(PUSH1, numCmpInterrupt1, FALLING);
    pinMode(PUSH2, INPUT_PULLUP);
    attachInterrupt(PUSH2, numCmpInterrupt2, FALLING);
  }
}

/* Send true if numbers are equal, false if different */
static void numCmpInterrupt1(void)
{
  ble.authKey = 1;
  Event_post(apEvent, AP_EVT_NUM_CMP_BTN);
}

static void numCmpInterrupt2(void)
{
  ble.authKey = 0;
  Event_post(apEvent, AP_EVT_NUM_CMP_BTN);
}

/*
 * Even though many events and resposes are asynchronous, we still handle them
 * synchronously. Any request that generates an asynchronous response should
 * Event_pend on the corresponding Event_post here.
 */
static void AP_asyncCB(uint8_t cmd1, void *pParams)
{
  switch (SNP_GET_OPCODE_HDR_CMD1(cmd1))
  {
    case SNP_DEVICE_GRP:
    {
      switch (cmd1)
      {
        case SNP_POWER_UP_IND:
        {
          logAsync("SNP_POWER_UP_IND", cmd1);
          // Notify state machine of Power Up Indication
          // Log_info0("Got PowerUp indication from NP");
          Event_post(apEvent, AP_EVT_PUI);
        } break;
        case SNP_HCI_CMD_RSP:
        {
          logAsync("SNP_HCI_CMD_RSP", cmd1);
          snpHciCmdRsp_t *hciRsp = (snpHciCmdRsp_t *) pParams;
          ble.opcode = hciRsp->opcode;
          logParam("opcode", hciRsp->opcode);
          if (hciRsp->status == SNP_SUCCESS)
          {
            asyncRspData = (snp_msg_t *) hciRsp;
            Event_post(apEvent, AP_EVT_HCI_RSP);
            Event_pend(apEvent, AP_NONE, AP_EVT_COPIED_ASYNC_DATA,
                       AP_EVENT_PEND_TIMEOUT);
          }
          else
          {
            apPostError(hciRsp->status, "SNP_HCI_CMD_RSP");
          }
        } break;
        case SNP_TEST_RSP:
        {
          logAsync("SNP_TEST_RSP", cmd1);
          snpTestCmdRsp_t *testRsp = (snpTestCmdRsp_t *) pParams;
          asyncRspData = (snp_msg_t *) testRsp;
          logParam("memAlo", testRsp->memAlo);
          logParam("memMax", testRsp->memMax);
          logParam("memSize", testRsp->memSize);
          Event_post(apEvent, AP_EVT_TEST_RSP);
          Event_pend(apEvent, AP_NONE, AP_EVT_COPIED_ASYNC_DATA,
                     AP_EVENT_PEND_TIMEOUT);
          // No status code in response
        } break;
        default:
          break;
      }
    } break;
    case SNP_GAP_GRP:
    {
      switch (cmd1)
      {
        case SNP_SET_ADV_DATA_CNF:
        {
          logAsync("SNP_SET_ADV_DATA_CNF", cmd1);
          snpSetAdvDataCnf_t *advDataRsp = (snpSetAdvDataCnf_t *) pParams;
          if (advDataRsp->status == SNP_SUCCESS)
          {
            Event_post(apEvent, AP_EVT_ADV_DATA_RSP);
          }
          else
          {
            apPostError(advDataRsp->status, "SNP_SET_ADV_DATA_CNF");
          }
        } break;
        // Just a confirmation that the request update was sent.
        case SNP_UPDATE_CONN_PARAM_CNF:
        {
          logAsync("SNP_UPDATE_CONN_PARAM_CNF", cmd1);
          snpUpdateConnParamCnf_t *connRsp =
            (snpUpdateConnParamCnf_t *) pParams;
          if (connRsp->status == SNP_SUCCESS)
          {
            Event_post(apEvent, AP_EVT_CONN_PARAMS_CNF);
          }
          else
          {
            apPostError(connRsp->status, "SNP_UPDATE_CONN_PARAM_CNF");
          }
        } break;
        case SNP_SEND_AUTHENTICATION_DATA_RSP:
        {
          logAsync("SNP_SEND_AUTHENTICATION_DATA_RSP", cmd1);
          snpSetAuthDataRsp_t *authRsp = (snpSetAuthDataRsp_t *) pParams;
          if (authRsp->status == SNP_SUCCESS)
          {
            Event_post(apEvent, AP_EVT_AUTH_RSP);
          }
          else
          {
            apPostError(authRsp->status, "SNP_SEND_AUTHENTICATION_DATA_RSP");
          }
        }
        case SNP_SET_WHITE_LIST_POLICY_RSP:
        {
          logAsync("SNP_SET_WHITE_LIST_POLICY_RSP", cmd1);
          snpSetWhiteListRsp_t *whiteListRsp = (snpSetWhiteListRsp_t *) pParams;
          if (whiteListRsp->status == SNP_SUCCESS)
          {
            Event_post(apEvent, AP_EVT_WHITE_LIST_RSP);
          }
          else
          {
            apPostError(whiteListRsp->status, "SNP_SET_WHITE_LIST_POLICY_RSP");
          }
        }
        default:
          break;
      }
    }
    case SNP_GATT_GRP:
    {
      switch (cmd1)
      {
        case SNP_SEND_NOTIF_IND_CNF:
        {
          logAsync("SNP_SEND_NOTIF_IND_CNF", cmd1);
          snpNotifIndCnf_t *notifIndRsp = (snpNotifIndCnf_t *) pParams;
          if (notifIndRsp->status == SNP_SUCCESS)
          {
            Event_post(apEvent, AP_EVT_NOTIF_IND_RSP);
          }
          else
          {
            apPostError(notifIndRsp->status, "SNP_SEND_NOTIF_IND_CNF");
          }
        } break;
        default:
          break;
      }
    }
    default:
      break;
  }
}

static void processSNPEventCB(uint16_t cmd1, snpEventParam_t *param)
{
  switch (cmd1)
  {
    case SNP_CONN_EST_EVT:
    {
      logAsync("SNP_CONN_EST_EVT", cmd1);
      snpConnEstEvt_t *evt = (snpConnEstEvt_t *) param;
      _connHandle                           = evt->connHandle;
      ble.usedConnParams.connInterval       = evt->connInterval;
      ble.usedConnParams.slaveLatency       = evt->slaveLatency;
      ble.usedConnParams.supervisionTimeout = evt->supervisionTimeout;
      logParam("connInterval", evt->connInterval);
      logParam("slaveLatency", evt->slaveLatency);
      logParam("supervisionTimeout", evt->supervisionTimeout);
      memcpy(&ble.bleAddr, &(evt->pAddr), sizeof(evt->pAddr));
      connected = true;
      Event_post(apEvent, AP_EVT_CONN_EST);
    } break;
    case SNP_CONN_TERM_EVT:
    {
      logAsync("SNP_CONN_TERM_EVT", cmd1);
      connected = false;
      Event_post(apEvent, AP_EVT_CONN_TERM);
      BLE_resetCCCD();
    } break;
    case SNP_CONN_PARAM_UPDATED_EVT:
    {
      logAsync("SNP_CONN_PARAM_UPDATED_EVT", cmd1);
      snpUpdateConnParamEvt_t *evt = (snpUpdateConnParamEvt_t *) param;
      if (ble.usedConnParams.connInterval != evt->connInterval)
      {
        logParam("connInterval", evt->connInterval);
      }
      if (ble.usedConnParams.slaveLatency != evt->slaveLatency)
      {
        logParam("slaveLatency", evt->slaveLatency);
      }
      if (ble.usedConnParams.supervisionTimeout != evt->supervisionTimeout)
      {
        logParam("supervisionTimeout", evt->supervisionTimeout);
      }
      ble.usedConnParams.connInterval       = evt->connInterval;
      ble.usedConnParams.slaveLatency       = evt->slaveLatency;
      ble.usedConnParams.supervisionTimeout = evt->supervisionTimeout;
      Event_post(apEvent, AP_EVT_CONN_PARAMS_UPDATED);
    } break;
    case SNP_ADV_STARTED_EVT:
    {
      logAsync("SNP_ADV_STARTED_EVT", cmd1);
      snpAdvStatusEvt_t *evt = (snpAdvStatusEvt_t *) param;
      if (evt->status == SNP_SUCCESS)
      {
        advertising = true;
        Event_post(apEvent, AP_EVT_ADV_ENB);
      }
      else
      {
        apPostError(evt->status, "SNP_ADV_STARTED_EVT");
      }
    } break;
    case SNP_ADV_ENDED_EVT:
    {
      logAsync("SNP_ADV_ENDED_EVT", cmd1);
      snpAdvStatusEvt_t *evt = (snpAdvStatusEvt_t *) param;
      if (evt->status == SNP_SUCCESS) {
        advertising = false;
        Event_post(apEvent, AP_EVT_ADV_END);
      }
      else
      {
        apPostError(evt->status, "SNP_ADV_ENDED_EVT");
      }
    } break;
    case SNP_ATT_MTU_EVT:
    {
      logAsync("SNP_ATT_MTU_EVT", cmd1);
      snpATTMTUSizeEvt_t *evt = (snpATTMTUSizeEvt_t *) param;
      ble.mtu = evt->attMtuSize - 3; // -3 for non-user data
      logParam("mtu", ble.mtu);
    } break;
    case SNP_SECURITY_EVT:
    {
      logAsync("SNP_SECURITY_EVT", cmd1);
      snpSecurityEvt_t *evt = (snpSecurityEvt_t *) param;
      ble.securityState = evt->state;
      logParam("state", evt->state);
      if (evt->status == SNP_SUCCESS) {
        Event_post(apEvent, AP_EVT_SECURITY_STATE);
      }
      else
      {
        apPostError(evt->status, "SNP_SECURITY_EVT");
      }
    } break;
    case SNP_AUTHENTICATION_EVT:
    {
      logAsync("SNP_AUTHENTICATION_EVT", cmd1);
      memcpy(&eventHandlerData, param, sizeof(eventHandlerData));
      Event_post(apEvent, AP_EVT_HANDLE_AUTH_EVT);
    } break;
    case SNP_ERROR_EVT:
    {
      logAsync("SNP_ERROR_EVT", cmd1);
      snpErrorEvt_t *evt = (snpErrorEvt_t *) param;
      ble.opcode = evt->opcode;
      logParam("Opcode", evt->opcode);
      apPostError(evt->status, "SNP_ERROR_EVT");
    } break;
  }
}

/*
 * Can't return specific error beacuse that's only in the async handler,
 * so we return true/false and let caller check ble.error.
 */
static bool apEventPend(uint32_t event)
{
  ble.error = BLE_SUCCESS;
  uint32_t postedEvent = Event_pend(apEvent, AP_NONE, event | AP_ERROR,
                                    AP_EVENT_PEND_TIMEOUT);
  /* Bug in NPI causes this specific event to get posted twice */
  if (event == AP_EVT_ADV_DATA_RSP)
  {
    Event_pend(apEvent, AP_NONE, event | AP_ERROR, AP_EVENT_PEND_TIMEOUT);
  }
  bool status = false;
  if (postedEvent & event)
  {
    status = true;
  }
  else if (postedEvent == 0)
  {
    ble.error = BLE_TIMEOUT;
    status = false;
  }
  else if (postedEvent & AP_ERROR)
  {
    // Function that posts AP_ERROR should set ble.error
    status = false;
  }
  return status;
}

static inline void apPostError(uint8_t status, const char errMsg[])
{
  logError(errMsg, status);
  ble.error = status;
  Event_post(apEvent, AP_ERROR);
}

/*
 * Handles propogating errors through stack to Energia sketch. Use when
 * failure of the checked call requires immediate return (e.g. if the
 * next statements depend on it). This preserves ble.error when it already
 * has been set to something besides BLE_SUCCESS, and otherwise sets it
 * to status.
 */
static bool isError(uint8_t status)
{
  if (status == BLE_CHECK_ERROR)
  {
    return true;
  }
  else if ((ble.error = status) != SNP_SUCCESS)
  {
    logError(status);
    return true;
  }
  return false;
}

void BLE::setLogLevel(uint8_t newLogLevel)
{
  logLevel = newLogLevel;
}