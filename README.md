# Ver os dados da ATCA

Este programa mostra no terminal os 16 valores `int32` mais recentes da placa
9. Lê apenas o buffer RT já existente em `/dev/atca_v6_dmart_9`.

Não usa MARTe, `ioctl`, IRQ, trigger ou configuração da placa. Abrir e mapear
este nó é passivo segundo a implementação do driver. Se o stream não estiver
ativo, o programa não o inicia: mostra valores inalterados e termina com um
aviso.

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
