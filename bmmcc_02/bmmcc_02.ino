/*
* Blackmagic Micro Cinema Camera Controller + LIDAR Autofocus 
* v0.2
*
* 2019-01-01 by buddley
* built for Teensy LC board
*/
#define ENCODER_OPTIMIZE_INTERRUPTS

#include <BMC_SBUS.h>
#include <EEPROM.h>
#include <Wire.h>
//#include <PWMServo.h> //use PWMServo when reg servo is unreliable
#include <Servo.h>
#include <Encoder.h>
#include <Bounce.h>
#include <Arduino.h>
#include <U8x8lib.h>
#include <SPI.h>
#include <spline.h>

/* pin */
#define RUN 2
#define ENCCLK 3
#define ENCDT 4
#define ENCPUSH 5
/* RX3 7  TX3 8  RX2 9  TX2 10
MOSI 11  SCK 13 */
#define BATT A0
#define REMOTE A1
#define FOCUS A2
#define SERVO A3
#define RST A8
#define DC A9
#define CS A10 // not in use

#define ADCRES 4095

/* Sbus def */
#define SBUS_MID 1024
#define SBUS_LOW 352
#define SBUS_HIGH 1696

BMC_SBUS sbus;

/* oled */
String line[4];
int disp_lines = 4;

U8X8_SSD1306_128X64_NONAME_4W_HW_SPI u8x8(CS, DC, RST);

/* encoder & buttons */
Encoder encoder(ENCCLK, ENCDT);
Bounce encbtn(ENCPUSH, 10);
Bounce recbtn(RUN, 50);
Bounce remotebtn(REMOTE, 10);

/* lidar */
bool lidar_conn = false;
int raw_dist, raw_prev, strength, checksum;
int uart[9];
const int leading = 0x59;
int dist = 200;
int dist_buffer[5];
int offset = 8;
int range = 200; //cm +/-range to follow focus 
int sens = 5; // 1-10 false_count > sens *50 ~ 0.5sec 
int false_count = 0;
int tail_count = 0;
#define TAIL 3

bool prev_af = false;
bool af = false;
bool ftin = true;

/* lens - hardcoded */
#define LENS 3
#define SCALE 12
String lens[LENS][2] = {
    {"24-85mm f/3.5", "min-7m(23ft)"},
    {"50mm f/1.4", "min-1m(3.3ft)"},
    {"50mm f/1.4", "0.65m(2.2ft)-inf"}
    };
/* lens scale and servo position
* actual measurement needs 12 positions
* first and last values are not used - only for spline calculation*/
int measurement[LENS] = {12,10,14};
int scale[LENS][14] = {
    {34,35,70,100,150,200,300,400,500,600,700,701},//10+2
    {44,45,50,55,60,70,80,90,100,102},//8+2
    {64,65,70,80,90,100,120,150,200,300,500,1000,5357,5358}//12+2
    }; //the second last one = inf, one of values = 1200cm = longest distance lidar can measure
int pos[LENS][14] = {
    {2500,2400,1475,1210,980,870,740,680,635,610,550,544},
    {2500,2400,1990,1730,1470,1130,900,740,565,544},
    {2500,2400,2200,2000,1800,1665,1440,1250,1060,900,755,645,544,500}
    };

Spline af_curve;
Servo servo;
int focus = 1500;
int prev_wheel;
#define UNHOLD 40

/* cam control */
float voltage = 0;
elapsedMillis volt_check;
#define V_REFESH 5000
int sbus_value[18] = {SBUS_MID, SBUS_MID, SBUS_MID, 0,0,0,SBUS_MID,0,0,0,0,0,0,0,0,0,0,0}; //0-2047 (11bit)
int aud = 70;
elapsedMillis lockup;
#define REC_HOLD 1000


/* menu */
bool lock = true;
bool edit = false;
int curr_menu = 0;
int curr_fps = 0;
int curr_codec = 0;
int curr_lens = 0;

#define MENU 10
String menu[] = {"ASA", "Shutter Angle", "White Balance", "Framerate", "Codec", "Lenses", "Sensitivity", "Focus Motor", "LIDAR Offset", "LIDAR Range"};
#define FPS 8
String fps[] = {"23.98p", "24.00p", "25.00p", "29.97p", "30.00p", "50.00p", "59.94p", "60.00p"};
#define CODEC 6
String codec[] = {"RAW", "RAW 3:1", "P422 HQ", "P422", "P422 LT", "P PROXY"};

/* FUNCTIONS */
// eeprom related
void read_rom(){
    int addr = 0;
    if (EEPROM.read(addr) != 255){
        addr++;
        EEPROM.get(addr, offset);
        addr += sizeof(offset);
        EEPROM.get(addr, range);
        addr += sizeof(range);
        EEPROM.get(addr, sens);
        addr += sizeof(sens);
        EEPROM.get(addr, aud);
    }
}
void write_rom(){
    int addr = 0;
    EEPROM.write(addr, 127);
    addr++;
    EEPROM.put(addr, offset);
    addr += sizeof(offset);
    EEPROM.put(addr, range);
    addr += sizeof(range);
    EEPROM.put(addr, sens);
    addr += sizeof(sens);
    EEPROM.put(addr, aud);
}

//spline
int map_spline(){
    int count = measurement[curr_lens];
    float x[count];
    float y[count];
    for (int i = 0; i < count; i++){
        x[i] = (float)scale[curr_lens][i];
        y[i] = (float)pos[curr_lens][i];
    }
    af_curve.setPoints(x, y, count);
    af_curve.setDegree( Catmull );
    return int(af_curve.value(float(dist)));
/*  for( int i = scale[curr_lens][0]; i < scale[curr_lens][SCALE - 1]; i+= 10 ) {
    float temp = af_curve.value(float(i));
    Serial.print(i);
    Serial.print("-->");
    Serial.println(int(temp));
  }*/
}
            
// control related
void step(int dir, int channel){
    channel++; //0-base to 1-base
    sbus.Servo(channel,SBUS_MID);
    sbus.Update();
    sbus.Send();
    delay(100);
    if (dir > 0){
        sbus.Servo(channel,SBUS_HIGH - 200);
        sbus.Update();
        sbus.Send();
    } else if (dir < 0){
        sbus.Servo(channel,SBUS_LOW + 200);
        sbus.Update();
        sbus.Send();
    }
    delay(100);
    sbus.Servo(channel,SBUS_MID);
    sbus.Update();
    sbus.Send();
}
void set_value(){
    switch (curr_menu){
        case 3:
            sbus.Servo((curr_menu + 1), int(SBUS_LOW + (SBUS_HIGH - SBUS_LOW) / FPS * curr_fps + 112));
            break;
        case 4:
            sbus.Servo((curr_menu + 1), int(SBUS_LOW + (SBUS_HIGH - SBUS_LOW) / CODEC * curr_codec + 84));
            break;
        case 5:
            //map_spline();
            break;
    }
    sbus.Update();
    sbus.Send();
    write_rom();
}

// display related
void welcome(){
    u8x8.clear();
    u8x8.setFont(u8x8_font_7x14_1x2_r);
    u8x8.setCursor(0,0);
    u8x8.println(" BMMCC CONTROL");
    u8x8.setCursor(0,2);
    u8x8.println("LIDAR AUTOFOCUS");
    u8x8.setCursor(0,4);
    u8x8.println("   d-project");
    u8x8.setCursor(0,6);
    u8x8.println("     190215");
    delay(2000);
}
void check_volt(){
    if (volt_check > V_REFESH){
        voltage = (float)analogRead(BATT) * 9.9 / (float)ADCRES; //9.9v -> v_div -> 3.3v
        volt_check = 0;
        if (lock){
            lock_screen();
        } else if (!lock){
            show_menu();
        }
    }
}
void lock_screen(){
    disp_lines = 3;
    if (lidar_conn){
        line[0] = "LOCK  " + String(voltage, 2) + "V A" + String(aud);
        if (af) {
            line[1] = "AUTO FOCUS";
        } else if(!af) {
            line[1] = "MANUAL FOCUS";
        }
        if (!ftin) {
            line[2] = String(dist) + "cm";
        } else if (ftin) {
            float ft = (float)dist / 2.54 / 12;
            float in = (float)dist / 2.54 - (int)ft * 12;
            line[2] = String((int)ft) + "ft" + String((int)in) + "in";
        }
    } else if (!lidar_conn){
        line[0] = "LOCK        A" + String(aud);
        line[1] = "BATTERY";
        line[2] = String(voltage, 2) + "V";
    }
    update_disp();
}
void show_menu(){
    line[0] = "MENU  " + String(voltage, 2) + "V A" + aud;
    if (!edit){ //scroll menu browsing
        disp_lines = 4;
        if (curr_menu != 0){
            line[1] = menu[curr_menu - 1];
        } else {
            line[1] = "";
        }
        line[2] = ">" + menu[curr_menu] + "<";
        if (curr_menu != (MENU - 1)){
            line[3] = menu[curr_menu + 1];
        } else {
            line[3] = "";
        }
    } else if (edit){ //edit mode
        disp_lines = 3;
        line[1] = menu[curr_menu];
        if (curr_menu < 3){ //up-down
            line[2] = "-/+";
        } else if (curr_menu == 3){
            line[2] = fps[curr_fps];
        } else if (curr_menu == 4){
            line[2] = codec[curr_codec];
        } else if (curr_menu == 5){
            disp_lines = 4;
            line[2] = lens[curr_lens][0];
            line[3] = lens[curr_lens][1];
        } else if (curr_menu == 6){
            line[2] = String(sens) + "/10";
        } else if (curr_menu == 7){
            line[2] = String(servo.readMicroseconds());
        } else if (curr_menu == 8){
            line[2] = String(offset) + "cm";
        } else if (curr_menu == 9){
            line[2] = String(range) + "cm";
        }
    }
    update_disp();    
}
void padding(int ln, bool big = false){
    int pad;
    char font[17];
    line[ln].toCharArray(font, 17);
    if (big){
        pad = (8 - strlen(font)) / 2;
    } else {
        pad = (16 - strlen(font)) / 2;
    }
    for (int i = 0; i < pad; i++){
        line[ln] = " " + line[ln] + "  ";
    }
}
void update_disp(){
    u8x8.setFont(u8x8_font_7x14_1x2_r);
    padding(0);
    u8x8.setCursor(0,0);
    u8x8.setInverseFont(1);
    u8x8.println(line[0]);
    //Serial.println(line[0]);
    padding(1);
    u8x8.setCursor(0,2);
    u8x8.setInverseFont(0);
    u8x8.println(line[1]);
    //Serial.println(line[1]);
    if (disp_lines == 4){
        padding(2);
        u8x8.setCursor(0,4);
        u8x8.println(line[2]);
        //Serial.println(line[2]);
        padding(3);
        u8x8.setCursor(0,6);
        u8x8.println(line[3]);
        //Serial.println(line[3]);
    } else {
        char big_font[9];
        u8x8.setFont(u8x8_font_inr21_2x4_r);
        padding(2, true);
        line[2].toCharArray(big_font, 9);
        u8x8.drawString(0,4,big_font);
        //Serial.println(line[2]);
    }
}

// button related
void record(){
    if (lockup > REC_HOLD){
        step(1, 6);
        Serial.println("Record Pressed");
        lockup = 0;
    } 
}
void check_button(){
    bool long_press = false;
    if (encbtn.update()){
        if (encbtn.fallingEdge()){
            Serial.println("Encoder pressed");
            if (!digitalRead(ENCPUSH)){
                unsigned long curr = millis();
                while (!digitalRead(ENCPUSH)){
                    if ((millis() - curr) > 1000){ // long push
                        long_press = true;
                        flip_lock();
                    }
                }
            }
            if (!long_press){
                button_handler(); //short push
            }
        }
    }
    if (recbtn.update()){
            if (recbtn.fallingEdge()){
                record();
            }
        }
}
void flip_lock(){
    if (lock){
        lock = false;
        show_menu();
    } else {
        lock = true;
        lock_screen();
    }
    edit = false;
    delay(1500);
}
void button_handler(){
    if (!lock){
        if (edit){ //edit mode
            edit = false;
            set_value();
        } else if (!edit){ //menu browsing
            edit = true;
        }
        show_menu();
    } else if (lock){
        if (ftin){
            ftin = false;
        } else if (!ftin){
            ftin = true;
        }
        lock_screen();
    }
}

// encoder related
void enc_interrupt(){
    int movement = encoder.read() / 4; // one notch = 4 pulse;
    if (movement != 0){
        Serial.print("Encoder move: ");
        Serial.println(movement);
        if (lock){ //when locked change audio
            aud = constrain((aud + movement), 0, 100);
            Serial.println(aud);
            sbus_value[5] = map(aud, 0, 100, SBUS_LOW, SBUS_HIGH);
            sbus.Servo(6, sbus_value[5]);
            sbus.Update();
            sbus.Send();
            write_rom();
            lock_screen();
        } else if (!lock){
            if (!edit){
                curr_menu = constrain((curr_menu + movement), 0, (MENU - 1));//nav menu system - free move
            } else if (edit){ //change values
                if (curr_menu < 3){
                    step(movement, curr_menu);// up down
                } else if (curr_menu == 3){
                    curr_fps = constrain((curr_fps + movement), 0, (FPS - 1));
                } else if (curr_menu == 4){
                    curr_codec = constrain((curr_codec + movement), 0, (CODEC - 1));
                } else if (curr_menu == 5){
                    curr_lens = constrain((curr_lens + movement), 0, (LENS - 1));
                } else if (curr_menu == 6){
                    sens = constrain((sens + movement), 1, 10);
                } else if (curr_menu == 8){
                    offset = constrain((offset + movement), -50, 50);
                } else if (curr_menu == 9){
                    range = constrain((range + movement), 0, 1200);
                }
            }
            Serial.println(curr_menu);
            show_menu();
        }
    encoder.write(0);
    }
}

// Autofocus
void read_lidar(){
    if (Serial3.read() == leading){
        uart[0] = leading;
        if (Serial3.read() == leading){
            uart[1] = leading;
            for (int i = 2; i < 9; i++){
                uart[i] = Serial3.read();
            }
            checksum = uart[0]+uart[1]+uart[2]+uart[3]+uart[4]+uart[5]+uart[6]+uart[7];
            if (uart[8] == (checksum & 0xff)){
                raw_dist = uart[3]*256 + uart[2] + offset;
                strength = uart[5]*256 + uart[4];
                if (20 < strength) {
                    if (abs(raw_prev - raw_dist) < range || false_count > (sens * 50)){
                        if (false_count > 0){
                            tail_count++;
                        }
                        if (false_count == 0 || tail_count > TAIL){
                            dist = dist - (dist - raw_dist) / 2; //unit: cm smoothing by 0.5
                            raw_prev = raw_dist;
                            dist_buffer[4] = dist_buffer[3];
                            dist_buffer[3] = dist_buffer[2];
                            dist_buffer[2] = dist_buffer[1];
                            dist_buffer[1] = dist_buffer[0];
                            dist_buffer[0] = dist;
                            false_count = 0;
                            tail_count = 0;
                        }
                    } else {
                        false_count++;
                        dist = dist_buffer[4];
                    }
                    if (lock){
                        lock_screen();
                    }
                }
            }
        }
    }
}
void servo_drive(){
    int wheel = analogRead(FOCUS);
    if (remotebtn.update() && remotebtn.risingEdge()){
        prev_wheel = wheel;
    }
    if (!digitalRead(REMOTE)){ // auto focus
        af = true;
        prev_af = true;
        focus = map_spline();
    } else { //manual focus
        af = false;
        if (prev_af && abs(prev_wheel - wheel) > UNHOLD){
            focus = map(wheel, 0, ADCRES, pos[curr_lens][1], pos[curr_lens][measurement[curr_lens] - 2]);
            prev_af = false;
            prev_wheel = wheel;
        } else if (!prev_af){
            focus = map(wheel, 0, ADCRES, pos[curr_lens][1], pos[curr_lens][measurement[curr_lens] - 2]);
            prev_wheel = wheel;
        }
        Serial.print(wheel);
        Serial.print("->");
    }
    Serial.println(focus);
    servo.writeMicroseconds(focus);
    if (!lock && curr_menu == 7 && edit){
        line[2] = String(servo.readMicroseconds());
        update_disp();
    }
}

/* execution */

void setup(){
    Serial.begin(115200); //USB debug
    //Serial1.begin(115200); //not in use. reserved.
    //Serial2.begin(100000); //sbus init by library
    Serial3.begin(115200); //lidar
    sbus.begin();
    
    u8x8.begin();
    delay(1000);
    welcome();
    
    read_rom();

    analogReadRes(12);
    pinMode(RUN, INPUT_PULLUP);
    pinMode(ENCPUSH, INPUT_PULLUP);
    pinMode(REMOTE, INPUT_PULLUP);

    /*
    pinMode(ENCCLK, INPUT_PULLUP);
    pinMode(ENCDT, INPUT_PULLUP);
    
    pinMode(BATT, INPUT);
    pinMode(FOCUS, INPUT);
    
    attachInterrupt(digitalPinToInterrupt(RUN), record, FALLING); //active low
    attachInterrupt(digitalPinToInterrupt(ENCCLK), enc_interrupt, CHANGE);
    attachInterrupt(digitalPinToInterrupt(ENCDT), enc_interrupt, CHANGE);
    attachInterrupt(digitalPinToInterrupt(ENCPUSH), check_button, FALLING);
    */

    servo.attach(SERVO);

    //initial sbus data either loaded from eeprom or default
    for(int i = 0; i < 18; i++){
        sbus.Servo(i,sbus_value[i]);
    }
    sbus.Update();

    map_spline();

    lock_screen();
}

void loop(void){
    if (Serial3.available() > 0){
        read_lidar();
        lidar_conn = true;
    } else {
        lidar_conn = false;
    }

    servo_drive();
    
    enc_interrupt();
    check_button();

    check_volt();
}
