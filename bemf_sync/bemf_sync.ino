#define F_CPU 1600000
/* Фоновый измеритель частоты BEMF при вращении мотора внешней силой.
 * Код выполняет только подсчет импульсов и вывод в UART.
 */

#define BTN_START PB0

#define SD_A PB3
#define SD_B PB2
#define SD_C PB1

#define IN_A PD5
#define IN_B PD4
#define IN_C PD3

#define SD_A_ON() PORTB = (PORTB & ~((1 << SD_B) | (1 << SD_C))) | (1 << SD_A)
#define SD_B_ON() PORTB = (PORTB & ~((1 << SD_A) | (1 << SD_C))) | (1 << SD_B)
#define SD_C_ON() PORTB = (PORTB & ~((1 << SD_A) | (1 << SD_B))) | (1 << SD_C)

#define IN_A_ON() PORTD = (PORTD & ~((1 << IN_B) | (1 << IN_C))) | (1 << IN_A)
#define IN_B_ON() PORTD = (PORTD & ~((1 << IN_A) | (1 << IN_C))) | (1 << IN_B)
#define IN_C_ON() PORTD = (PORTD & ~((1 << IN_A) | (1 << IN_B))) | (1 << IN_C)

volatile unsigned long bldc_pulses_counter = 0;  // Счетчик импульсов BEMF
unsigned long last_uart_time = 0;                // Таймер вывода в UART
unsigned long last_btn_start = 0;
byte bldc_step = 0;  // Текущий шаг для мультиплексора

void setup() {
  Serial.begin(9600);  // Инициализация UART
  Serial.println("Start");

  // DDRD |= 0x38;  // Configure pins 3, 4 and 5 as outputs
  // Настройка пинов IN_C, IN_B и IN_A на вывод (OUTPUT)
  DDRD |= (1 << IN_C) | (1 << IN_B) | (1 << IN_A);
  PORTD = 0x00;
  // DDRB |= 0x0E;  // Configure pins 9, 10 and 11 as outputs
  // PORTB = 0x31;
  // Настройка пинов SD_C, SD_B, SD_A на вывод
  DDRB |= (1 << SD_C) | (1 << SD_B) | (1 << SD_A);
  // Установка HIGH на пинах PB0, PB4, PB5 (и LOW на SD_C, SD_B, SD_A)
  // PORTB = (1 << PB5) | (1 << PB4) | (1 << PB0);
  PORTB = 0x00;
  PORTB |= (1 << BTN_START);  //pull up
  // Timer1 module setting: set clock source to clkI/O / 1 (no prescaling)
  TCCR1A = 0;
  // Настройка TCCR1B:
  // Биты 4:3 -> 00 (WGM13=0, WGM12=0)  Завершаем настройку режима Phase Correct PWM 8-bit (Режим 1)
  // Биты 2:0 -> 001 (CS12=0, CS11=0, CS10=1) Предделитель = 1 (тактование напрямую от кварца)
  // TCCR1B = (1 << CS11) | (1 << CS10); // Предделитель на 64 (для ATmega328P)
  // TCCR1B = (1 << CS12);               // Предделитель на 256
  TCCR1B = (1 << CS10);  // В шестнадцатеричном виде это 0x01
  // Timer2 module setting: set clock source to clkI/O / 1 (no prescaling)
  TCCR2A = 0;
  // Настройка TCCR2B:
  // Бит 3    -> 0  (WGM22=0) Завершаем настройку режима Phase Correct PWM
  // Биты 2:0 -> 001 (CS22=0, CS21=0, CS20=1) Предделитель = 1
  TCCR2B = (1 << CS20);  // В шестнадцатеричном виде это 0x01

  // Analog comparator setting
  ACSR = 0x10;  // Disable and clear (flag bit) analog comparator interrupt

  // Первоначальная настройка мультиплексора на фазу B для симуляции сбоя фаз
  BEMF_A_RISING();

  ACSR |= 0x08;  //Сразу включаем прерывания компаратора
  // delay(100);
}

volatile bool forward = true;
volatile bool can_run = false;

void loop() {
  byte PWM_A, PWM_B, PWM_C;
  PWM_A = PWM_B = PWM_C = 50;
  // PWM_C = 120;
  OCR1A = PWM_C;
  OCR1B = PWM_B;
  OCR2A = PWM_A;
  // OCR2B  = 120;

  if (millis() - last_btn_start >= 100) {
    last_btn_start = millis();
    if (!(PINB & (1 << BTN_START))) {
      // Код выполнится, если на BTN_START сейчас LOW (кнопка нажата на землю)
      can_run = true;
    } else {
      can_run = false;
    }
  }

  if (!can_run) {
    TCCR2A = 0;  // Полностью отключаем ШИМ 2
    TCCR1A = 0;

    // // 1. Отключаем выводы пинов от таймера 1 и таймера 2
    // TCCR1A &= ~((1 << COM1A1) | (1 << COM1B1));
    // TCCR2A &= ~((1 << COM2A1) | (1 << COM2B1));

    // TCCR1A = (1 << COM1A1) | (1 << COM1B1) | (1 << WGM10);
    // TCCR2A = (1 << COM2A1) | (1 << COM2B1) | (1 << WGM20);
    // OCR1A = 0;  //PWM_C;
    // OCR1B = 0;  //PWM_B;
    // OCR2A = 0;  //PWM_A;
    // OCR2B = 0;
    // 2. Явно прижимаем пины ШИМ к земле
    PORTB = (PORTB & ~((1 << SD_A) | (1 << SD_B) | (1 << SD_C)));
  }

  // Отправка данных в UART ровно один раз в секунду
  if (millis() - last_uart_time >= 1000) {
    last_uart_time = millis();
    unsigned long current_pulses = 0;

    // Атомарное копирование счетчика с временным запретом прерываний
    noInterrupts();
    current_pulses = bldc_pulses_counter;
    bldc_pulses_counter = 0;  // Сброс для подсчета за следующую секунду
    interrupts();


    if (forward)
      Serial.print('+');
    else
      Serial.print('-');
    // Вывод чистого значения частоты импульсов в секунду
    Serial.println(current_pulses / 6);
  }
}

// Прерывание аналогового компаратора по BEMF
ISR(ANALOG_COMP_vect) {
  // BEMF debounce
  for (int i = 0; i < 10; i++) {
    if (bldc_step & 1) {
      if (!(ACSR & 0x20)) i -= 1;
    } else {
      if ((ACSR & 0x20)) i -= 1;
    }
  }
  bldc_pulses_counter++;  // Считаем импульс

  ADCSRA = (0 << ADEN);  // Disable the ADC module
  bldc_move();
  bldc_step++;

  bool expected_rising = !(bldc_step & 1);  // Четные шаги (0,2,4) ждут RISING
  bool actual_rising = (ACSR & (1 << ACO));

  if (actual_rising == expected_rising) {
    // Мотор крутится ВПЕРЕД
    forward = true;
  } else {
    // Мотор крутится НАЗАД
    forward = false;
    //todo stop
  }

  bldc_step %= 6;
}
void bldc_move() {  // BLDC motor commutation function
  if (can_run) {
    switch (bldc_step) {
      case 0:
        AH_BL();
        break;
      case 1:
        AH_CL();
        break;
      case 2:
        BH_CL();
        break;
      case 3:
        BH_AL();
        break;
      case 4:
        CH_AL();
        break;
      case 5:
        CH_BL();
        break;
    }
  }

  switch (bldc_step) {
    case 0:
      // AH_BL();
      BEMF_C_RISING();
      break;
    case 1:
      // AH_CL();
      BEMF_B_FALLING();
      break;
    case 2:
      // BH_CL();
      BEMF_A_RISING();
      break;
    case 3:
      // BH_AL();
      BEMF_C_FALLING();
      break;
    case 4:
      // CH_AL();
      BEMF_B_RISING();
      break;
    case 5:
      // CH_BL();
      BEMF_A_FALLING();
      break;
  }
}

void BEMF_A_RISING() {
  // ADCSRA = (0 << ADEN);  // Disable the ADC module
  ADCSRB = (0 << ACME);  // Select AIN1 as comparator negative input
  ACSR |= 0x03;          // Set interrupt on rising edge
  // Настройка прерывания компаратора по НАРАСТАЮЩЕМУ фронту (биты ACIS1=1, ACIS0=1)
  // ACSR |= (1 << ACIS1) | (1 << ACIS0);
}
void BEMF_A_FALLING() {
  // ADCSRA = (0 << ADEN);  // Disable the ADC module
  ADCSRB = (0 << ACME);  // Select AIN1 as comparator negative input
  ACSR &= ~0x01;         // Set interrupt on falling edge
  // Настройка прерывания по СПАДАЮЩЕМУ фронту (бит ACIS1=1, бит ACIS0=0)
  // ACSR |= (1 << ACIS1);
  // ACSR &= ~(1 << ACIS0);
  // ACSR = (ACSR & ~(1 << ACIS0)) | (1 << ACIS1);
}
void BEMF_B_RISING() {
  // ADCSRA = (0 << ADEN);  // Disable the ADC module
  ADCSRB = (1 << ACME);  //Подключаем мультиплексор АЦП
  ADMUX = 2;             // Select analog channel 2 as comparator negative input
  ACSR |= 0x03;
}
void BEMF_B_FALLING() {
  // ADCSRA = (0 << ADEN);  // Disable the ADC module
  ADCSRB = (1 << ACME);
  ADMUX = 2;  // Select analog channel 2 as comparator negative input
  ACSR &= ~0x01;
}
void BEMF_C_RISING() {
  // ADCSRA = (0 << ADEN);  // Disable the ADC module
  ADCSRB = (1 << ACME);
  ADMUX = 3;  // Select analog channel 3 as comparator negative input
  ACSR |= 0x03;
}
void BEMF_C_FALLING() {
  // ADCSRA = (0 << ADEN);  // Disable the ADC module
  ADCSRB = (1 << ACME);
  ADMUX = 3;  // Select analog channel 3 as comparator negative input
  ACSR &= ~0x01;
}
//------------------
void AH_BL() {
  // Turn pin 11 (OC2A) PWM ON (pin 9 & pin 10 OFF)
  // PORTB  =  0x04;
  // PORTD &= ~0x18;
  // PORTD |=  0x20;
  // TCCR1A =  0;
  // TCCR2A =  0x81;
  SD_B_ON();  // Включаем только SD_B, остальные SD в 0
  // сбрасываем IN_C, IN_B и устанавливаем IN_A
  // (PORTD & ~((1 << IN_B) | (1 << IN_C))) — безопасное закрытие ключей на IN_C и IN_B (замена &= ~0x18)
  // | (1 << IN_A) — открытие ключа на IN_A (замена |= 0x20)
  IN_A_ON();
  //Отключаем ШИМ 1. Пины SD_C (OC1A) и SD_B (OC1B)
  TCCR1A = 0;
  TCCR2A = (1 << COM2A1) | (1 << WGM20);  //  0x81
  // Non-inverting ШИМ для обоих каналов (COM2A1=1 и COM2B1=1) + Phase Correct (WGM20=1)
  // TCCR2A = (1 << COM2A1) | (1 << COM2B1) | (1 << WGM20);  // В шестнадцатеричном виде это 0xA1
}
void AH_CL() {
  // Turn pin 11 (OC2A) PWM ON (pin 9 & pin 10 OFF)
  // PORTB = 0x02;
  // PORTD &= ~0x18;
  // PORTD |= 0x20;
  // TCCR1A = 0;
  SD_C_ON();  // Включаем только SD_C, остальные SD в 0
  // Одновременно: сбрасываем IN_C, IN_B в ноль и устанавливаем IN_A в 1
  IN_A_ON();
  TCCR1A = 0;                             // Полностью отключаем ШИМ 1
  TCCR2A = (1 << COM2A1) | (1 << WGM20);  // 0x81 Включается Phase Correct ШИМ на канале А Таймера 2 (OC2A / пин SD_A).
}
void BH_CL() {
  // Turn pin 10 (OC1B) PWM ON (pin 9 & pin 11 OFF)
  // PORTB  =  0x02;
  // PORTD &= ~0x28;
  // PORTD |= 0x10;
  // TCCR2A = 0;
  SD_C_ON();  // Включаем только SD_C, остальные SD в 0
  // Одновременно: сбрасываем IN_C, IN_A в ноль и устанавливаем IN_B в 1
  IN_B_ON();
  TCCR2A = 0;  // Полностью отключаем ШИМ на Таймере 2
  // Настройка TCCR1A:
  // Биты 5:4 -> 10 (COM1B1=1, COM1B0=0) -> Clear OC1B on Compare Match when up-counting, set when down-counting
  // Биты 1:0 -> 01 (WGM11=0, WGM10=1)    -> Режим Phase Correct PWM, 8-bit
  TCCR1A = (1 << COM1B1) | (1 << WGM10);  // В шестнадцатеричном виде это 0x21
}
void BH_AL() {
  // Turn pin 10 (OC1B) PWM ON (pin 9 & pin 11 OFF)
  // PORTB = 0x08;
  // PORTD &= ~0x28;
  // PORTD |= 0x10;
  // TCCR2A = 0;
  SD_A_ON();  // Включаем SD_A, остальные SD в 0
  // Одновременно: сбрасываем IN_C, IN_A в ноль и устанавливаем IN_B в 1
  IN_B_ON();
  TCCR2A = 0;  // Полностью отключаем ШИМ на Таймере 2
  // Настройка TCCR1A:
  // Биты 5:4 -> 10 (COM1B1=1, COM1B0=0) -> Clear OC1B on Compare Match when up-counting, set when down-counting
  // Биты 1:0 -> 01 (WGM11=0, WGM10=1)    -> Режим Phase Correct PWM, 8-bit
  TCCR1A = (1 << COM1B1) | (1 << WGM10);  // В шестнадцатеричном виде это 0x21
}
void CH_AL() {
  // Turn pin 9 (OC1A) PWM ON (pin 10 & pin 11 OFF)
  // PORTB = 0x08;
  // PORTD &= ~0x30;
  // PORTD |= 0x08;
  // TCCR2A = 0;
  SD_A_ON();  // Включаем SD_A, остальные SD в 0
  // Одновременно: сбрасываем IN_B, IN_A в ноль и устанавливаем IN_C в 1
  IN_C_ON();
  TCCR2A = 0;                             // Полностью отключаем ШИМ 2
  TCCR1A = (1 << COM1A1) | (1 << WGM10);  // 0x81;
}
void CH_BL() {
  // Turn pin 9 (OC1A) PWM ON (pin 10 & pin 11 OFF)
  // PORTB = 0x04;
  // PORTD &= ~0x30;
  // PORTD |= 0x08;
  // TCCR2A = 0;
  SD_B_ON();  // Включаем только SD_B, остальные SD в 0
  // Одновременно: сбрасываем IN_B, IN_A в ноль и устанавливаем IN_C в 1
  IN_C_ON();
  TCCR2A = 0;                             // Полностью отключаем ШИМ 2
  TCCR1A = (1 << COM1A1) | (1 << WGM10);  // 0x81
}