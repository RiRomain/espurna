/*

ITEAD RF BRIDGE MODULE

Copyright (C) 2017 by Xose Pérez <xose dot perez at gmail dot com>

*/

#ifdef ITEAD_SONOFF_RFBRIDGE

#include <vector>
#include <Ticker.h>

// -----------------------------------------------------------------------------
// DEFINITIONS
// -----------------------------------------------------------------------------

#define RF_MESSAGE_SIZE         9
#ifndef RF_RAW_SUPPORT
#  define RF_MAX_MESSAGE_SIZE   RF_MESSAGE_SIZE
#else
#  undef  RF_MAX_MESSAGE_SIZE
#  define RF_MAX_MESSAGE_SIZE   16 // (112+4)
#endif
#define RF_CODE_START           0xAA
#define RF_CODE_ACK             0xA0
#define RF_CODE_LEARN           0xA1
#define RF_CODE_LEARN_KO        0xA2
#define RF_CODE_LEARN_OK        0xA3
#define RF_CODE_RFIN            0xA4
#define RF_CODE_RFOUT           0xA5
#define RF_CODE_SNIFFING_ON     0xA6
#define RF_CODE_SNIFFING_OFF    0xA7
#define RF_CODE_RFOUT_NEW       0xA8
#define RF_CODE_LEARN_NEW       0xA9
#define RF_CODE_LEARN_KO_NEW    0xAA
#define RF_CODE_LEARN_OK_NEW    0xAB
#define RF_CODE_RFOUT_BUCKET    0xB0
#define RF_CODE_STOP            0x55

// -----------------------------------------------------------------------------
// GLOBALS TO THE MODULE
// -----------------------------------------------------------------------------

unsigned char _uartbuf[RF_MESSAGE_SIZE+3] = {0};
unsigned char _uartpos = 0;
unsigned char _learnId = 0;
bool _learnStatus = true;
bool _rfbin = false;

typedef struct {
    byte code[RF_MAX_MESSAGE_SIZE];
    byte times;
#ifdef RF_RAW_SUPPORT
    byte length;
#endif
} rfb_message_t;
std::vector<rfb_message_t> _rfb_message_queue;
Ticker _rfbTicker;

// -----------------------------------------------------------------------------
// PRIVATES
// -----------------------------------------------------------------------------

void _rfbWebSocketOnSend(JsonObject& root) {
    root["rfbVisible"] = 1;
    root["rfbCount"] = relayCount();
    JsonArray& rfb = root.createNestedArray("rfb");
    for (byte id=0; id<relayCount(); id++) {
        for (byte status=0; status<2; status++) {
            JsonObject& node = rfb.createNestedObject();
            node["id"] = id;
            node["status"] = status;
            node["data"] = rfbRetrieve(id, status == 1);
        }
    }
}

void _rfbWebSocketOnAction(const char * action, JsonObject& data) {
    if (strcmp(action, "rfblearn") == 0) rfbLearn(data["id"], data["status"]);
    if (strcmp(action, "rfbforget") == 0) rfbForget(data["id"], data["status"]);
    if (strcmp(action, "rfbsend") == 0) rfbStore(data["id"], data["status"], data["data"].as<const char*>());
}

void _rfbAck() {
    DEBUG_MSG_P(PSTR("[RFBRIDGE] Sending ACK\n"));
    Serial.println();
    Serial.write(RF_CODE_START);
    Serial.write(RF_CODE_ACK);
    Serial.write(RF_CODE_STOP);
    Serial.flush();
    Serial.println();
}

void _rfbLearn() {

    DEBUG_MSG_P(PSTR("[RFBRIDGE] Sending LEARN\n"));
    Serial.println();
    Serial.write(RF_CODE_START);
    Serial.write(RF_CODE_LEARN);
    Serial.write(RF_CODE_STOP);
    Serial.flush();
    Serial.println();

    #if WEB_SUPPORT
        char buffer[100];
        snprintf_P(buffer, sizeof(buffer), PSTR("{\"action\": \"rfbLearn\", \"data\":{\"id\": %d, \"status\": %d}}"), _learnId, _learnStatus ? 1 : 0);
        wsSend(buffer);
    #endif

}

void _rfbSendRaw(const byte *message, const unsigned char n = RF_MESSAGE_SIZE) {
    for (unsigned char j=0; j<n; j++) {
        Serial.write(message[j]);
    }
}

void _rfbSend(byte * message) {
    Serial.println();
    Serial.write(RF_CODE_START);
    Serial.write(RF_CODE_RFOUT);
    _rfbSendRaw(message);
    Serial.write(RF_CODE_STOP);
    Serial.flush();
    Serial.println();
}

void _rfbSend() {

    // Check if there is something in the queue
    if (_rfb_message_queue.size() == 0) return;

    // Pop the first element
    rfb_message_t message = _rfb_message_queue.front();
    _rfb_message_queue.erase(_rfb_message_queue.begin());

    // Send the message
#ifdef RF_RAW_SUPPORT
    bool sendRaw = message.times < 0;
    int length = sendRaw ? -message.times : message.times;
    if (sendRaw) {
        _rfbSendRaw(message.code, message.length);
        Serial.flush();
    }
    else
#endif
    _rfbSend(message.code);

    // If it should be further sent, push it to the stack again
    if (message.times > 1) {
        message.times = message.times - 1;
        _rfb_message_queue.push_back(message);
    }
#ifdef RF_RAW_SUPPORT
    else if (message.times < -1) {
        message.times = message.times + 1;
        _rfb_message_queue.push_back(message);
    }
#endif

    // if there are still messages in the queue...
    if (_rfb_message_queue.size() > 0) {
        _rfbTicker.once_ms(RF_SEND_DELAY, _rfbSend);
    }

}

void _rfbSend(byte * code, int times) {

    char buffer[RF_MESSAGE_SIZE];
    _rfbToChar(code, buffer);
    DEBUG_MSG_P(PSTR("[RFBRIDGE] Sending MESSAGE '%s' %d time(s)\n"), buffer, times);

    rfb_message_t message;
    memcpy(message.code, code, RF_MESSAGE_SIZE);
    message.times = times;
#ifdef RF_RAW_SUPPORT
    message.length = RF_MESSAGE_SIZE;
#endif
    _rfb_message_queue.push_back(message);
    _rfbSend();

}

#ifdef RF_RAW_SUPPORT
void _rfbSendRawRepeated(byte *code, int length, int times) {
    char buffer[RF_MESSAGE_SIZE*2];
    _rfbToChar(code, buffer);
    DEBUG_MSG_P(PSTR("[RFBRIDGE] Sending raw MESSAGE '%s' %d time(s)\n"), buffer, times);

    rfb_message_t message;
    memcpy(message.code, code, length);
    message.times = -times;
    message.length = length;
    _rfb_message_queue.push_back(message);
    _rfbSend();
}
#endif

bool _rfbMatch(char * code, unsigned char& relayID, unsigned char& value) {

    if (strlen(code) != 18) return false;

    bool found = false;
    String compareto = String(&code[12]);
    compareto.toUpperCase();
    DEBUG_MSG_P(PSTR("[RFBRIDGE] Trying to match code %s\n"), compareto.c_str());

    for (unsigned char i=0; i<relayCount(); i++) {

        String code_on = rfbRetrieve(i, true);
        if (code_on.length() && code_on.endsWith(compareto)) {
            DEBUG_MSG_P(PSTR("[RFBRIDGE] Match ON code for relay %d\n"), i);
            value = 1;
            found = true;
        }

        String code_off = rfbRetrieve(i, false);
        if (code_off.length() && code_off.endsWith(compareto)) {
            DEBUG_MSG_P(PSTR("[RFBRIDGE] Match OFF code for relay %d\n"), i);
            if (found) value = 2;
            found = true;
        }

        if (found) {
            relayID = i;
            return true;
        }

    }

    return false;

}

void _rfbDecode() {

    static unsigned long last = 0;
    if (millis() - last < RF_RECEIVE_DELAY) return;
    last = millis();

    byte action = _uartbuf[0];
    char buffer[RF_MESSAGE_SIZE * 2 + 1] = {0};
    DEBUG_MSG_P(PSTR("[RFBRIDGE] Action 0x%02X\n"), action);

    if (action == RF_CODE_LEARN_KO) {
        _rfbAck();
        DEBUG_MSG_P(PSTR("[RFBRIDGE] Learn timeout\n"));
        #if WEB_SUPPORT
            wsSend_P(PSTR("{\"action\": \"rfbTimeout\"}"));
        #endif
    }

    if (action == RF_CODE_LEARN_OK || action == RF_CODE_RFIN) {
        #if MQTT_SUPPORT
            _rfbToChar(&_uartbuf[1], buffer);
            mqttSend(MQTT_TOPIC_RFIN, buffer);
        #endif
        _rfbAck();
    }

    if (action == RF_CODE_LEARN_OK) {

        DEBUG_MSG_P(PSTR("[RFBRIDGE] Learn success\n"));
        rfbStore(_learnId, _learnStatus, buffer);

        // Websocket update
        #if WEB_SUPPORT
            char wsb[100];
            snprintf_P(wsb, sizeof(wsb), PSTR("{\"rfb\":[{\"id\": %d, \"status\": %d, \"data\": \"%s\"}]}"), _learnId, _learnStatus ? 1 : 0, buffer);
            wsSend(wsb);
        #endif

    }

    if (action == RF_CODE_RFIN) {

        DEBUG_MSG_P(PSTR("[RFBRIDGE] Forward message '%s'\n"), buffer);

        // Look for the code
        unsigned char id;
        unsigned char status = 0;
        if (_rfbMatch(buffer, id, status)) {
            _rfbin = true;
            if (status == 2) {
                relayToggle(id);
            } else {
                relayStatus(id, status == 1);
            }
        }

    }

}

void _rfbReceive() {

    static bool receiving = false;

    while (Serial.available()) {

        yield();
        byte c = Serial.read();
        //DEBUG_MSG_P(PSTR("[RFBRIDGE] Received 0x%02X\n"), c);

        if (receiving) {
            if (c == RF_CODE_STOP) {
                _rfbDecode();
                receiving = false;
            } else {
                _uartbuf[_uartpos++] = c;
            }
        } else if (c == RF_CODE_START) {
            _uartpos = 0;
            receiving = true;
        }

    }


}

bool _rfbCompare(const char * code1, const char * code2) {
    return strcmp(&code1[12], &code2[12]) == 0;
}

bool _rfbSameOnOff(unsigned char id) {
    return _rfbCompare(rfbRetrieve(id, true).c_str(), rfbRetrieve(id, false).c_str());
}

/*
From an hexa char array ("A220EE...") to a byte array (half the size)
 */
int _rfbToArray(const char * in, byte * out, int length = RF_MESSAGE_SIZE * 2) {
    int n = strlen(in);
    if (n > RF_MAX_MESSAGE_SIZE*2 || (length > 0 && n != length)) return 0;
    char tmp[3] = {0,0,0};
    n /= 2;
    for (unsigned char p = 0; p<n; p++) {
        memcpy(tmp, &in[p*2], 2);
        out[p] = strtol(tmp, NULL, 16);
    }
    return n;
}

/*
From a byte array to an hexa char array ("A220EE...", double the size)
 */
bool _rfbToChar(byte * in, char * out) {
    for (unsigned char p = 0; p<RF_MESSAGE_SIZE; p++) {
        sprintf_P(&out[p*2], PSTR("%02X"), in[p]);
    }
    return true;
}

#if MQTT_SUPPORT
void _rfbMqttCallback(unsigned int type, const char * topic, const char * payload) {

    if (type == MQTT_CONNECT_EVENT) {
        char buffer[strlen(MQTT_TOPIC_RFLEARN) + 3];
        snprintf_P(buffer, sizeof(buffer), PSTR("%s/+"), MQTT_TOPIC_RFLEARN);
        mqttSubscribe(buffer);
        mqttSubscribe(MQTT_TOPIC_RFOUT);
    }

    if (type == MQTT_MESSAGE_EVENT) {

        // Match topic
        String t = mqttSubtopic((char *) topic);

        // Check if should go into learn mode
        if (t.startsWith(MQTT_TOPIC_RFLEARN)) {

            _learnId = t.substring(strlen(MQTT_TOPIC_RFLEARN)+1).toInt();
            if (_learnId >= relayCount()) {
                DEBUG_MSG_P(PSTR("[RFBRIDGE] Wrong learnID (%d)\n"), _learnId);
                return;
            }
            _learnStatus = (char)payload[0] != '0';
            _rfbLearn();

        }

#ifdef RF_RAW_SUPPORT
        bool isRFOut = t.equals(MQTT_TOPIC_RFOUT);
        bool isRFRaw = !isRFOut && t.equals(MQTT_TOPIC_RFRAW);
        if (isRFOut || isRFRaw) {
#else
        if (t.equals(MQTT_TOPIC_RFOUT)) {
#endif
            // The payload may be a code in HEX format ([0-9A-Z]{18}) or
            // the code comma the number of times to transmit it.
            byte message[RF_MAX_MESSAGE_SIZE];
            char * tok = strtok((char *) payload, ",");

            // Check if a switch is linked to that message
            unsigned char id;
            unsigned char status = 0;
            if (_rfbMatch(tok, id, status)) {
                if (status == 2) {
                    relayToggle(id);
                } else {
                    relayStatus(id, status == 1);
                }
                return;
            }

            const char *tok2 = strtok(NULL, ",");
            byte times = (t != NULL) ? atoi(tok2) : 1;
#ifdef RF_RAW_SUPPORT
            int len = _rfbToArray(tok, message, 0);
            if (len > 0 && (isRFRaw || len != RF_MESSAGE_SIZE)) {
                _rfbSendRawRepeated(message, len, times);
            } else {
#else
            if (_rfbToArray(tok, message)) {
#endif
                _rfbSend(message, times);
            }

        }

    }

}
#endif

// -----------------------------------------------------------------------------
// PUBLIC
// -----------------------------------------------------------------------------

void rfbStore(unsigned char id, bool status, const char * code) {
    DEBUG_MSG_P(PSTR("[RFBRIDGE] Storing %d-%s => '%s'\n"), id, status ? "ON" : "OFF", code);
    char key[8] = {0};
    snprintf_P(key, sizeof(key), PSTR("rfb%s%d"), status ? "ON" : "OFF", id);
    setSetting(key, code);
}

String rfbRetrieve(unsigned char id, bool status) {
    char key[8] = {0};
    snprintf_P(key, sizeof(key), PSTR("rfb%s%d"), status ? "ON" : "OFF", id);
    return getSetting(key);
}

void rfbStatus(unsigned char id, bool status) {
    String value = rfbRetrieve(id, status);
    if (value.length() > 0) {
        bool same = _rfbSameOnOff(id);
        byte message[RF_MESSAGE_SIZE];
        _rfbToArray(value.c_str(), message);
        unsigned char times = RF_SEND_TIMES;
        if (same) times = _rfbin ? 0 : 1;
        _rfbSend(message, times);
    }
}

void rfbLearn(unsigned char id, bool status) {
    _learnId = id;
    _learnStatus = status;
    _rfbLearn();
}

void rfbForget(unsigned char id, bool status) {

    char key[8] = {0};
    snprintf_P(key, sizeof(key), PSTR("rfb%s%d"), status ? "ON" : "OFF", id);
    delSetting(key);

    // Websocket update
    #if WEB_SUPPORT
        char wsb[100];
        snprintf_P(wsb, sizeof(wsb), PSTR("{\"rfb\":[{\"id\": %d, \"status\": %d, \"data\": \"\"}]}"), id, status ? 1 : 0);
        wsSend(wsb);
    #endif

}

// -----------------------------------------------------------------------------
// SETUP & LOOP
// -----------------------------------------------------------------------------

void rfbSetup() {

    #if MQTT_SUPPORT
        mqttRegister(_rfbMqttCallback);
    #endif

    #if WEB_SUPPORT
        wsOnSendRegister(_rfbWebSocketOnSend);
        wsOnActionRegister(_rfbWebSocketOnAction);
    #endif

}

void rfbLoop() {
    _rfbReceive();
}

#endif
