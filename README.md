# Ver os dados da ATCA

Este programa mostra no terminal os 16 valores `int32` mais recentes da placa
9. Lê apenas o buffer RT já existente em `/dev/atca_v6_dmart_9`.

Não usa MARTe, aquisição raw, IRQ ou trigger. Se o fluxo RT estiver desligado,
o programa liga apenas o bit `StreamE`, mostra os dados e volta a desligá-lo no
fim. Se já estiver ligado, deixa-o ligado. O programa recusa alterar o stream
se detetar uma aquisição ativa.

No servidor, basta executar:

```bash
./run.sh
```

Por omissão mostra 20 leituras, separadas por 500 ms. Opcionalmente:

```bash
./run.sh PLACA NUMERO_DE_LEITURAS INTERVALO_MS
```

Por exemplo, placa 9, 10 leituras, uma a cada segundo:

```bash
./run.sh 9 10 1000
```

Os valores são os inteiros disponibilizados pelo fluxo RT do FPGA, sem
calibração para volts. Este fluxo já foi decimado pelo FPGA; não contém todas
as amostras originais a 2 MHz.
