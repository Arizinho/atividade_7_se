# Sistema de Irrigação Inteligente com BitDogLab

Este projeto foi desenvolvido utilizando a placa BitDogLab. Seu objetivo é automatizar a irrigação com base na umidade do solo e no nível de água disponível no reservatório, utilizando sensores simulados por joystick e comunicação via protocolo MQTT.

O sistema funciona da seguinte forma:

* **Modo Automático (tópico `/irrigacao/auto_on`):** ativa a bomba automaticamente quando a umidade estiver abaixo do valor configurado (tópico `/irrigacao/umidade_min`), desde que o nível de água esteja acima de 10%.
* **Modo Manual (tópico `/irrigacao/turn_on`):** permite ao usuário acionar manualmente a bomba de irrigação.
* **Alerta de nível baixo:** se o nível do reservatório estiver abaixo de 10%, o sistema desliga a bomba e emite um alerta sonoro e visual.

Recursos visuais e sonoros:

* **Display OLED:** exibe os valores de umidade e nível em tempo real.
* **Matriz de LEDs:** exibe uma exclamação (!) em vermelho quando o nível estiver crítico.
* **LED Azul:** acende para simular a ativação da bomba de irrigação.
* **Buzzer (PWM):** emite som de alerta quando há pouca água.

Os dados são publicados periodicamente nos seguintes tópicos MQTT:

* `/dados/umidade`
* `/dados/nivel`

Todas as funcionalidades foram implementadas como **tarefas independentes com FreeRTOS**, utilizando **filas, semáforos e mutex**.
