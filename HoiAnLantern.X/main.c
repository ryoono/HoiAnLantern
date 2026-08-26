/*
 * ============================================================
 *  Hoi An Lantern
 *  PIC16F18313 LED Flicker Program
 * ============================================================
 *
 * MCU      : PIC16F18313
 * Compiler : MPLAB XC8
 * Supply   : AA battery x 3 (nominal 4.5 V)
 * Clock    : Internal HFINTOSC 1 MHz
 *
 * ------------------------------------------------------------
 * Pin assignment
 * ------------------------------------------------------------
 *
 * PIC16F18313
 *
 * Pin 1 : VDD
 * Pin 2 : RA5 / PWM6 -> 100R -> 2SK4017 Gate (LED CH2)
 * Pin 3 : RA4 / PWM5 -> 100R -> 2SK4017 Gate (LED CH1)
 * Pin 4 : MCLR/VPP
 * Pin 5 : RA2         -> unused
 * Pin 6 : RA1/ICSPCLK
 * Pin 7 : RA0/ICSPDAT
 * Pin 8 : VSS
 *
 * 2SK4017 Gate-GND : 100kR pull-down
 *
 * LED:
 * +4.5V -> current limiting resistor(75ohm) -> LED(OSM2DK5111A-UV) -> 2SK4017 Drain
 * 2SK4017 Source -> GND
 *
 * ------------------------------------------------------------
 * Flicker algorithm
 * ------------------------------------------------------------
 *
 * LEDの揺らぎ生成部分は以下の記事で紹介されている
 * 間欠カオスを用いた手法をベースにしている。
 *
 * https://www.creativity-ape.com/entry/2020/12/16/232928
 *
 * 元のArduino版から以下を変更している。
 *
 * ・float演算をQ15整数演算に変更
 * ・PIC16F18313のPWM5 / PWM6を使用
 * ・Timer2によるハードウェアPWM
 * ・delay()を使用しない
 * ・CPUは待機中Idleモードへ移行
 * ・不要な周辺モジュールをPMDで停止
 *
 * ============================================================
 */

#include <xc.h>
#include <stdint.h>


// ============================================================
// Configuration Bits
// ============================================================

// 外部発振器を使用しない
#pragma config FEXTOSC = OFF

// リセット後はHFINTOSC 1MHzで起動
#pragma config RSTOSC = HFINT1

// クロック出力を無効化
#pragma config CLKOUTEN = OFF

// ソフトウェアからのクロック切替を許可
#pragma config CSWEN = ON

// 外部クロックを使わないのでFail-Safe Clock Monitorは不要
#pragma config FCMEN = OFF


// MCLRを有効化
#pragma config MCLRE = ON

// 電源投入時にPower-up Timerを使用
#pragma config PWRTE = ON

// Watchdog Timerは使用しない
#pragma config WDTE = OFF

// Brown-out Resetを有効化
#pragma config BOREN = ON

// 低いBOR電圧を使用
#pragma config BORV = LOW

// PPSロック解除は1回のみ
#pragma config PPS1WAY = ON

// Stack Overflow / Underflow時にReset
#pragma config STVREN = ON

// デバッグ機能OFF
#pragma config DEBUG = OFF


// Low Voltage Programming有効
#pragma config LVP = ON

// Flash自己書込み保護なし
#pragma config WRT = OFF

// Program Memory保護なし
#pragma config CP = OFF

// EEPROM保護なし
#pragma config CPD = OFF


#define _XTAL_FREQ 1000000UL


// ============================================================
// User settings
// ============================================================

/*
 * 揺らぎの深さ
 *
 * 元Arduino版:
 *   dimming_range = 50
 *
 * と同じ。
 *
 * 大きくすると明暗差が大きくなる。
 */
#define FLICKER_DEPTH          50u


/*
 * 全体の明るさ
 *
 * 256 = 100%
 * 192 =  75%
 * 128 =  50%
 *  64 =  25%
 *
 * 電池寿命を延ばすなら、
 * PIC側の省電力化よりこの値を下げる方が圧倒的に効く。
 *
 * 最初は100%で実物を確認する。
 */
#define MASTER_BRIGHTNESS_Q8   256u


/*
 * 間欠カオスのthreshold
 *
 * 元Arduino版:
 *
 * threshold = 0.065
 *
 * Q15では
 *
 * 0.065 * 32768 ≒ 2130
 */
#define CHAOS_THRESHOLD        2130u

#define Q15_ONE                32768UL
#define Q15_HALF               16384u


/*
 * 擬似乱数初期値
 *
 * 2個のランタンに完全に同じプログラムを書き込むと、
 * 同時起動時に同じ揺らぎになる可能性がある。
 *
 * 2台目ではこの値を変更するとよい。
 */
#define RNG_SEED               0xA531u


// ============================================================
// Gamma correction table
// ============================================================

/*
 * 人間の目の明るさ感覚に合わせるためのgamma補正。
 *
 * 元Arduino版と同じテーブルを使用する。
 */

static const uint8_t gamma8[256] = {
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1,
    1, 1, 1, 1, 1, 1, 1, 1, 1, 2, 2, 2, 2, 2, 2, 2,
    2, 3, 3, 3, 3, 3, 3, 3, 4, 4, 4, 4, 4, 5, 5, 5,
    5, 6, 6, 6, 6, 7, 7, 7, 7, 8, 8, 8, 9, 9, 9, 10,
    10, 10, 11, 11, 11, 12, 12, 13, 13, 13, 14, 14, 15, 15, 16, 16,
    17, 17, 18, 18, 19, 19, 20, 20, 21, 21, 22, 22, 23, 24, 24, 25,
    25, 26, 27, 27, 28, 29, 29, 30, 31, 32, 32, 33, 34, 35, 35, 36,
    37, 38, 39, 39, 40, 41, 42, 43, 44, 45, 46, 47, 48, 49, 50, 50,
    51, 52, 54, 55, 56, 57, 58, 59, 60, 61, 62, 63, 64, 66, 67, 68,
    69, 70, 72, 73, 74, 75, 77, 78, 79, 81, 82, 83, 85, 86, 87, 89,
    90, 92, 93, 95, 96, 98, 99, 101, 102, 104, 105, 107, 109, 110, 112, 114,
    115, 117, 119, 120, 122, 124, 126, 127, 129, 131, 133, 135, 137, 138, 140, 142,
    144, 146, 148, 150, 152, 154, 156, 158, 160, 162, 164, 167, 169, 171, 173, 175,
    177, 180, 182, 184, 186, 189, 191, 193, 196, 198, 200, 203, 205, 208, 210, 213,
    215, 218, 220, 223, 225, 228, 231, 233, 236, 239, 241, 244, 247, 249, 252, 255
};


// ============================================================
// Internal states
// ============================================================

/*
 * 間欠カオス内部値。
 *
 * Q15形式:
 *
 * 0     = 0.0
 * 16384 = 0.5
 * 32768 = 1.0
 *
 * 初期値0.10
 */
static uint16_t chaosValue = 3277u;


/*
 * PWMの目標Duty。
 *
 * PIC16F18313のPWMは10bit。
 *
 * 0 ～ 1023
 */
static uint16_t targetDuty5 = 0u;
static uint16_t targetDuty6 = 0u;


/*
 * 擬似乱数内部状態
 */
static uint16_t rngState = RNG_SEED;


/*
 * Timer2割り込み1回を1 tickとする。
 *
 * 今回:
 *
 * PWM周期     ≒ 1.024ms
 * Postscaler  = 1:16
 *
 * したがって
 *
 * 1 tick ≒ 16.384ms
 */
static uint8_t updateTicks = 1u;


/*
 * 起動フェード用。
 *
 * 約128tick = 約2.1秒
 */
static uint8_t fadeTicks = 0u;


// ============================================================
// Pseudo random generator
// ============================================================

/*
 * 16bit xorshift
 *
 * Arduinoのrandom()の代わりとして使用。
 *
 * 暗号用途ではなく、
 * LEDの揺らぎ周期を少し乱すだけなので十分。
 */
static uint16_t rand16(void)
{
    uint16_t x = rngState;

    x ^= (uint16_t)(x << 7);
    x ^= (uint16_t)(x >> 9);
    x ^= (uint16_t)(x << 8);

    /*
     * xorshiftは0が永久に0になるため、
     * 念のため0を回避する。
     */
    if (x == 0u) {
        x = 0xA531u;
    }

    rngState = x;

    return x;
}


// ============================================================
// PPS control
// ============================================================

/*
 * Peripheral Pin Selectのロック解除。
 *
 * PIC16F18313ではPWM出力をRA4/RA5へ割り当てるために
 * PPS設定が必要。
 */
static void unlockPPS(void)
{
    /*
     * PPS変更シーケンス中は割込み禁止。
     *
     * 今回はもともとGIE=0で運用するが、
     * 明示的に0へする。
     */
    INTCONbits.GIE = 0;

    PPSLOCK = 0x55;
    PPSLOCK = 0xAA;

    PPSLOCKbits.PPSLOCKED = 0;
}


/*
 * PPSを再ロックする。
 */
static void lockPPS(void)
{
    PPSLOCK = 0x55;
    PPSLOCK = 0xAA;

    PPSLOCKbits.PPSLOCKED = 1;
}


// ============================================================
// PWM register access
// ============================================================

/*
 * PWM5 duty設定。
 *
 * duty10:
 *   0 ～ 1023
 */
static void writePWM5(uint16_t duty10)
{
    if (duty10 > 1023u) {
        duty10 = 1023u;
    }

    /*
     * PWM5DC[9:2]
     */
    PWM5DCH = (uint8_t)(duty10 >> 2);

    /*
     * PWM5DC[1:0]はDCLのbit7:6に格納
     */
    PWM5DCL = (uint8_t)((duty10 & 0x03u) << 6);
}


/*
 * PWM6 duty設定。
 */
static void writePWM6(uint16_t duty10)
{
    if (duty10 > 1023u) {
        duty10 = 1023u;
    }

    PWM6DCH = (uint8_t)(duty10 >> 2);
    PWM6DCL = (uint8_t)((duty10 & 0x03u) << 6);
}


// ============================================================
// Brightness conversion
// ============================================================

/*
 * 8bit brightness:
 *
 * 0 ～ 255
 *
 * を
 *
 * PWM 10bit:
 *
 * 0 ～ 1023
 *
 * に変換する。
 *
 * 上位8bitをそのまま使用し、
 * 上位側のbitを下位2bitにも複製することで、
 *
 * 255 -> 1023
 *
 * になる。
 */
static uint16_t brightnessToDuty10(uint8_t brightness)
{
    return
        ((uint16_t)brightness << 2)
        |
        ((uint16_t)brightness >> 6);
}


/*
 * MASTER_BRIGHTNESSを適用する。
 *
 * MASTER_BRIGHTNESS_Q8 = 256
 *
 * のとき入力値をそのまま返す。
 */
static uint8_t applyMasterBrightness(uint8_t brightness)
{
    uint16_t temp;

    temp =
        (uint16_t)brightness
        *
        (uint16_t)MASTER_BRIGHTNESS_Q8;

    return (uint8_t)(temp >> 8);
}


// ============================================================
// Brightness calculation
// ============================================================

/*
 * 現在のchaosValueから
 * 2つのLEDのPWM Dutyを求める。
 *
 * CH1とCH2は逆方向に変化する。
 *
 * 片方が明るくなると、
 * もう片方が暗くなる。
 */
static void calculateBrightness(void)
{
    uint16_t delta;

    uint8_t raw5;
    uint8_t raw6;

    uint8_t gamma5;
    uint8_t gamma6;


    /*
     * chaosValue:
     *
     * 0 ～ 32768
     *
     * を
     *
     * 0 ～ FLICKER_DEPTH
     *
     * に変換する。
     */
    delta =
        (uint16_t)(
            ((uint32_t)chaosValue
             *
             (uint32_t)FLICKER_DEPTH)
            >> 15
        );


    /*
     * 元Arduino版:
     *
     * value_1 =
     *   255 - dimming_range
     *   + value * dimming_range;
     *
     * value_2 =
     *   255
     *   - value * dimming_range;
     *
     * と同じ。
     */
    raw5 =
        (uint8_t)(
            255u
            -
            FLICKER_DEPTH
            +
            delta
        );

    raw6 =
        (uint8_t)(
            255u
            -
            delta
        );


    /*
     * gamma補正
     */
    gamma5 = gamma8[raw5];
    gamma6 = gamma8[raw6];


    /*
     * 全体輝度設定を反映
     */
    gamma5 = applyMasterBrightness(gamma5);
    gamma6 = applyMasterBrightness(gamma6);


    /*
     * 8bit -> 10bit PWMへ変換
     */
    targetDuty5 = brightnessToDuty10(gamma5);
    targetDuty6 = brightnessToDuty10(gamma6);
}


// ============================================================
// Intermittent chaos
// ============================================================

/*
 * 間欠カオスの状態を1ステップ進める。
 *
 * 元Arduino版:
 *
 * if (value < 0.5) {
 *     value += 2 * value * value;
 * }
 * else {
 *     value -= 2 * (1-value) * (1-value);
 * }
 *
 * をQ15整数演算で実装。
 */
static void updateChaos(void)
{
    uint32_t square;


    if (chaosValue < Q15_HALF) {

        /*
         * value += 2 * value^2
         *
         * Q15なので、
         *
         * 2*x*x / 32768
         *
         * =
         *
         * x*x / 16384
         *
         * となる。
         */
        square =
            (uint32_t)chaosValue
            *
            (uint32_t)chaosValue;

        chaosValue +=
            (uint16_t)(square >> 14);
    }
    else {

        uint16_t t;

        /*
         * t = 1 - value
         */
        t =
            (uint16_t)(
                Q15_ONE
                -
                chaosValue
            );

        square =
            (uint32_t)t
            *
            (uint32_t)t;

        chaosValue -=
            (uint16_t)(square >> 14);
    }


    /*
     * 0または1付近まで来ると周期性が強くなるため、
     * thresholdを超えたところで乱数を再注入する。
     */
    if (
        (chaosValue <= CHAOS_THRESHOLD)
        ||
        (
            chaosValue
            >=
            (uint16_t)(
                Q15_ONE
                -
                CHAOS_THRESHOLD
            )
        )
    ) {

        uint16_t range;

        range =
            (uint16_t)(
                Q15_ONE
                -
                (2UL * CHAOS_THRESHOLD)
            );

        chaosValue =
            CHAOS_THRESHOLD
            +
            (uint16_t)(
                (
                    (uint32_t)rand16()
                    *
                    (uint32_t)range
                )
                >> 16
            );
    }


    /*
     * 新しいchaosValueからLEDの明るさを計算。
     */
    calculateBrightness();
}


// ============================================================
// Flicker timing
// ============================================================

/*
 * 次の明るさ更新までの時間をランダム化する。
 *
 * Timer2 interrupt tick:
 *
 * 約16.384ms
 *
 * 基本:
 *
 * 5～8 tick
 * ≒ 82～131ms
 *
 * さらに約1/3の確率で
 * 数tick追加する。
 *
 * 元Arduino版の
 *
 * 80～125ms
 *
 * と
 *
 * 時々 +40～140ms
 *
 * を近似している。
 */
static uint8_t getNextUpdateTicks(void)
{
    uint8_t ticks;

    uint16_t r;


    r = rand16();

    /*
     * 5 ～ 8 tick
     */
    ticks =
        (uint8_t)(
            5u
            +
            (r & 0x03u)
        );


    /*
     * およそ1/3の確率で
     * 少し長く停止させる。
     */
    if ((uint8_t)(rand16() & 0xFFu) < 85u) {

        /*
         * +3 ～ +10 tick
         *
         * 約49～164ms追加
         */
        ticks +=
            (uint8_t)(
                3u
                +
                (rand16() & 0x07u)
            );
    }

    return ticks;
}


// ============================================================
// Peripheral Module Disable
// ============================================================

/*
 * 使用しない周辺回路へのクロック供給を停止する。
 *
 * PWM5 / PWM6 / Timer2だけは残す。
 *
 * 消費電流への寄与はLEDに比べれば小さいが、
 * バッテリ機器なので不要なものは止めておく。
 */
static void disableUnusedPeripherals(void)
{
    /*
     * Fixed Voltage Reference
     * Clock Reference
     * Interrupt On Change
     */
    PMD0bits.FVRMD  = 1;
    PMD0bits.CLKRMD = 1;
    PMD0bits.IOCMD  = 1;


    /*
     * NCO、Timer0、Timer1停止。
     *
     * Timer2はPWMに必要なので有効のまま。
     */
    PMD1bits.NCOMD  = 1;
    PMD1bits.TMR0MD = 1;
    PMD1bits.TMR1MD = 1;
    PMD1bits.TMR2MD = 0;


    /*
     * Analog peripherals停止。
     */
    PMD2bits.DACMD  = 1;
    PMD2bits.ADCMD  = 1;
    PMD2bits.CMP1MD = 1;


    /*
     * PWM5 / PWM6は使用する。
     *
     * CCP、CWGは不要。
     */
    PMD3bits.CWG1MD = 1;

    PMD3bits.PWM5MD = 0;
    PMD3bits.PWM6MD = 0;

    PMD3bits.CCP1MD = 1;
    PMD3bits.CCP2MD = 1;


    /*
     * UART / SPI / I2C停止。
     */
    PMD4bits.UART1MD = 1;
    PMD4bits.MSSP1MD = 1;


    /*
     * CLC / DSM停止。
     */
    PMD5bits.CLC1MD = 1;
    PMD5bits.CLC2MD = 1;
    PMD5bits.DSMMD  = 1;
}


// ============================================================
// GPIO initialization
// ============================================================

static void initGPIO(void)
{
    /*
     * PORTAをすべてデジタルとして使用。
     */
    ANSELA = 0x00;


    /*
     * Open Drain無効。
     */
    ODCONA = 0x00;


    /*
     * Weak Pull-up無効。
     */
    WPUA = 0x00;


    /*
     * 出力LatchをLOWへしておく。
     */
    LATA = 0x00;


    /*
     * PWM初期化中はRA4 / RA5を入力にして、
     * LEDが不用意に点灯しないようにする。
     *
     * RA3 : MCLR
     * RA4 : input temporarily
     * RA5 : input temporarily
     *
     * RA0 / RA1 / RA2はLOW出力。
     *
     * RA0/RA1はICSP時にはプログラマ側が制御する。
     */
    TRISA = 0x38;
}


// ============================================================
// PWM initialization
// ============================================================

static void initPWM(void)
{
    /*
     * PWM5 / PWM6を一旦停止。
     *
     * 出力極性はactive-high。
     */
    PWM5CON = 0x00;
    PWM6CON = 0x00;


    /*
     * 初期Duty = 0
     */
    writePWM5(0u);
    writePWM6(0u);


    /*
     * ========================================================
     * Timer2
     * ========================================================
     *
     * FOSC = 1MHz
     *
     * PWM frequency =
     *
     * FOSC /
     * [4 * (PR2 + 1) * prescaler]
     *
     * PR2 = 255
     * prescaler = 1
     *
     * =>
     *
     * 1,000,000 /
     * (4 * 256)
     *
     * ≒ 976.56 Hz
     *
     * PR2=255なので10bit PWMを最大限使用できる。
     */


    PR2  = 255u;
    TMR2 = 0u;


    /*
     * Timer2 interrupt flag clear
     */
    PIR1bits.TMR2IF = 0;


    /*
     * T2CON
     *
     * bit6:3 T2OUTPS = 1111
     *                  Postscaler 1:16
     *
     * bit2 TMR2ON    = 1
     *
     * bit1:0 T2CKPS  = 00
     *                  Prescaler 1:1
     *
     * 0111 1100 = 0x7C
     */
    T2CON = 0x7C;


    /*
     * 最初のTimer2周期が完了するまで待つ。
     *
     * Datasheet推奨のPWM初期化手順。
     *
     * Postscalerが1:16なので
     * 約16ms待つことになるが、起動時だけなので問題ない。
     */
    while (PIR1bits.TMR2IF == 0) {
        ;
    }

    PIR1bits.TMR2IF = 0;


    /*
     * ========================================================
     * PPS
     * ========================================================
     *
     * PIC16F18313:
     *
     * PPS output source
     *
     * 0x02 = PWM5
     * 0x03 = PWM6
     *
     * RA4 <- PWM5
     * RA5 <- PWM6
     */

    unlockPPS();

    RA4PPS = 0x02;
    RA5PPS = 0x03;

    lockPPS();


    /*
     * PWM module enable。
     */
    PWM5CONbits.PWM5EN = 1;
    PWM6CONbits.PWM6EN = 1;


    /*
     * RA4 / RA5を出力へ変更。
     */
    TRISAbits.TRISA4 = 0;
    TRISAbits.TRISA5 = 0;


    /*
     * Timer2 interruptをIdle解除用として使用する。
     */
    PIR1bits.TMR2IF = 0;
    PIE1bits.TMR2IE = 1;


    /*
     * Peripheral Interrupt Enable
     */
    INTCONbits.PEIE = 1;


    /*
     * GIEは0のまま。
     *
     * これによりTimer2 interrupt requestでIdleから復帰するが、
     * ISRへは飛ばず、SLEEP()の次の命令から処理を再開する。
     */
    INTCONbits.GIE = 0;
}


// ============================================================
// Idle mode initialization
// ============================================================

static void initIdle(void)
{
    /*
     * IDLEN = 1
     *
     * SLEEP命令実行時、
     * Full SleepではなくIdleへ移行する。
     *
     * CPUとProgram Memoryは停止するが、
     * Timer2とPWMは継続動作する。
     */
    CPUDOZEbits.IDLEN = 1;
}


// ============================================================
// Main
// ============================================================

void main(void)
{
    uint8_t brightnessChanged;


    /*
     * GPIO初期化。
     */
    initGPIO();


    /*
     * 不要な周辺回路を停止。
     */
    disableUnusedPeripherals();


    /*
     * 初期の明るさを計算。
     */
    calculateBrightness();


    /*
     * PWM初期化。
     */
    initPWM();


    /*
     * 次回揺らぎ更新タイミングを決定。
     */
    updateTicks = getNextUpdateTicks();


    /*
     * Idle使用開始。
     */
    initIdle();


    /*
     * ========================================================
     * Main loop
     * ========================================================
     */

    while (1) {

        /*
         * ----------------------------------------------------
         * CPUをIdleへ
         * ----------------------------------------------------
         *
         * Timer2 / PWMはハードウェアで動作し続ける。
         *
         * 約16.4ms後、Timer2 interrupt requestによって
         * CPUが再開する。
         */
        SLEEP();
        NOP();


        /*
         * Timer2 interrupt flagをクリア。
         */
        if (PIR1bits.TMR2IF) {
            PIR1bits.TMR2IF = 0;
        }


        brightnessChanged = 0u;


        /*
         * ----------------------------------------------------
         * 揺らぎ更新タイミング
         * ----------------------------------------------------
         */

        if (updateTicks > 0u) {
            updateTicks--;
        }


        if (updateTicks == 0u) {

            /*
             * 間欠カオスを1ステップ進め、
             * 次のPWM Dutyを計算する。
             */
            updateChaos();


            /*
             * 次の更新タイミングをランダムに設定。
             */
            updateTicks = getNextUpdateTicks();


            brightnessChanged = 1u;
        }


        /*
         * ----------------------------------------------------
         * 起動フェードイン
         * ----------------------------------------------------
         *
         * 約2.1秒かけて0% -> 通常輝度へ上げる。
         */

        if (fadeTicks < 128u) {

            uint16_t out5;
            uint16_t out6;


            fadeTicks++;


            /*
             * targetDuty * fadeTicks / 128
             *
             * /128なのでシフト演算で処理。
             */
            out5 =
                (uint16_t)(
                    (
                        (uint32_t)targetDuty5
                        *
                        (uint32_t)fadeTicks
                    )
                    >> 7
                );

            out6 =
                (uint16_t)(
                    (
                        (uint32_t)targetDuty6
                        *
                        (uint32_t)fadeTicks
                    )
                    >> 7
                );


            writePWM5(out5);
            writePWM6(out6);
        }
        else {

            /*
             * フェード終了後は、
             * 揺らぎ値が変わったときだけPWMレジスタを書き換える。
             *
             * 無駄なCPU処理を減らす。
             */
            if (brightnessChanged) {

                writePWM5(targetDuty5);
                writePWM6(targetDuty6);
            }
        }
    }
}