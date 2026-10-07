# Como funciona a captura ATCA

Este documento explica o que acontece entre o sinal elétrico na entrada da
placa e o ficheiro criado por `capture_atca.cpp`. Assume conhecimentos de C++,
mas não conhecimentos de drivers, FPGA ou DMA.

## 1. Percurso dos dados

```text
Entrada analógica
      │
      ▼
16 ADCs — convertem tensão em códigos signed de 18 bits
      │
      ▼
FPGA — recebe os bits dos ADCs e forma um instante com 16 canais
      │
      ▼
DMA — transfere blocos da FPGA diretamente para a RAM do computador
      │
      ▼
Driver Linux — publica a interface /dev/atca_v6_9
      │
      ▼
capture_atca.cpp — read() de um buffer completo e write() para o .bin
```

O programa não comunica diretamente com os ADCs. Quando recebe os dados, estes
já passaram pela desserialização da FPGA e pelo controlador DMA.

## 2. O dispositivo `/dev/atca_v6_9`

Este caminho não é um ficheiro guardado num disco. É um *character device*
criado pelo driver da placa. As operações feitas no descritor são traduzidas
pelo kernel em operações do hardware:

- `open()` obtém acesso à placa;
- `ioctl()` consulta ou altera registos da FPGA;
- `read()` espera por um buffer DMA e copia-o para o programa;
- `close()` liberta o descritor.

O sufixo `9` identifica a placa usada por omissão. A opção `-b` permite escolher
outro número.

## 3. O que são os `ioctl`

Um `ioctl` é um comando específico de um driver. Neste programa são utilizados
para consultar o estado, limpar a fila DMA, ativar interrupções, armar a
aquisição e emitir um software trigger.

A sequência é importante:

```text
Consultar estado
      ↓
Confirmar que a placa está livre
      ↓
Reset da fila DMA
      ↓
Ativar IRQ, caso necessário
      ↓
Ativar aquisição
      ↓
Software trigger
      ↓
Ler buffers
      ↓
Desativar o que o programa ativou
```

Uma IRQ (*interrupt request*) é uma notificação do hardware ao processador. Em
vez de o programa perguntar continuamente se existem dados, a placa avisa o
driver quando um buffer está pronto.

O software trigger não gera os 2 milhões de instantes individualmente. Apenas
define o início da aquisição; depois a FPGA e os ADCs continuam ao ritmo dos
clocks do hardware.

## 4. O que é um buffer DMA

DMA permite que a placa transfira dados para RAM sem o processador copiar cada
amostra. O firmware observado usa buffers de 512 KiB.

Cada instante ocupa:

```text
16 canais × 4 bytes = 64 bytes
```

Por isso, um buffer contém:

```text
524288 / 64 = 8192 amostras por canal
```

A 2 MSamples/s, um buffer representa:

```text
8192 / 2000000 = 0,004096 s = 4,096 ms
```

É por isso que a duração final é arredondada para um múltiplo de 4,096 ms.

## 5. Formato do ficheiro binário

Os valores estão organizados por instante:

```text
instante 0: ch00 ch01 ch02 ... ch15
instante 1: ch00 ch01 ch02 ... ch15
instante 2: ch00 ch01 ch02 ... ch15
```

Cada elemento é uma palavra `int32` little-endian produzida pela FPGA. O código
ADC signed de 18 bits ocupa a parte superior:

```cpp
std::int32_t adc_code = fpga_word >> 14;
```

O shift tem de ser aritmético para preservar o sinal dos valores negativos.
Os bits inferiores podem transportar contadores, fase do chopper ou zeros,
consoante o canal. O capturador guarda a palavra completa para não perder essa
informação.

## 6. Por que existe um ficheiro JSON

O `.bin` não contém cabeçalho. Sem informação externa, seria impossível saber
com segurança quantos canais existem, a taxa ou como as palavras estão
organizadas. O `.json` regista:

- dispositivo e kernel;
- formato e ordem dos canais;
- taxa ADC;
- duração pedida e duração efetiva;
- tamanho e número de buffers;
- estado inicial da placa;
- se a captura terminou completamente.

O `.bin` e o `.bin.json` devem ser tratados como um par.

## 7. Proteções importantes do programa

Antes de começar, o capturador recusa trabalhar se encontrar aquisição, DMA ou
stream RT ativos. Isto evita interferir com MARTe ou outro processo.

O programa regista se ativou IRQ e aquisição. No fim, desativa apenas aquilo
que ele próprio iniciou. Uma IRQ que já estivesse ativa é preservada.

O ficheiro é criado com `O_EXCL`, portanto uma captura existente nunca é
substituída silenciosamente. Se ocorrer um erro ou `Ctrl+C`, o binário parcial é
mantido e o JSON contém `"complete": false`.

## 8. O que significa `driver_max_pending_buffers`

Enquanto a placa produz dados, o programa tem de os guardar suficientemente
depressa. O driver devolve o maior número de buffers que chegaram a estar
pendentes. Um valor baixo indica que o leitor acompanhou a placa.

Este valor não substitui a verificação do contador embutido pela FPGA. Para
confirmar continuidade absoluta é necessário também analisar esse contador.

## 9. Limites da captura raw

"Raw" significa que o programa não aplica calibração, integração ou decimação.
Contudo, os dados já passaram pelos ADCs e pela lógica de receção da FPGA. Uma
falha na desserialização de um canal já estará presente no `.bin` e não pode ser
recuperada pelo driver ou por este programa.
