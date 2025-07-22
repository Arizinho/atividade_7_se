# Estação Meteorológica com Raspberry Pi Pico W

Este projeto utiliza o Raspberry Pi Pico W para monitorar temperatura, umidade, pressão e altitude. Ele exibe as informações localmente em um display OLED, alerta com buzzer e matriz de LEDs e disponibiliza uma interface web.

O sistema funciona da seguinte forma:

* Leitura de sensores (AHT20 e BMP280): coleta temperatura, umidade e pressão atmosférica.
* Cálculo de altitude baseado na pressão medida em relação ao nível do mar.
* Interface Web: exibe valores em tempo real, gráficos e permite configurar valores limites para temperatura e umidade.
* Display OLED (SSD1306): mostra os valores atuais e o IP do dispositivo.
* Matriz de LEDs 5x5: acende em padrão de alerta .
* Buzzer PWM: emite alertas sonoros.
