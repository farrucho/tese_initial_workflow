# Captura raw da ATCA

Ferramenta autónoma para guardar os 16 canais raw da placa ATCA a 2 MSPS por
canal, sem MARTe. Usa a interface DMA de `/dev/atca_v6_9`.

## Utilização

```bash
./run.sh -t 1 -o dados.bin
```

- `-t`: duração pedida, em segundos;
- `-o`: ficheiro binário novo;
- `-b`: número da placa, por omissão `9`.

A aquisição termina num limite de buffer DMA. Com o buffer observado de
512 KiB, cada buffer contém 8192 amostras por canal e representa 4,096 ms.
Por isso, `-t 1` produz 1,00352 s, 2 007 040 amostras por canal e 122,5 MiB.
A captura mínima é de 16 buffers: 65,536 ms e 8 MiB.

É criado também `dados.bin.json`, com duração efetiva, tamanho do buffer,
número de amostras, estado inicial, chopper, kernel e formato.

## Formato

O binário contém valores `int32` little-endian, sem divisão por `2^14`:

```text
amostra 0: ch00 ch01 ... ch15
amostra 1: ch00 ch01 ... ch15
...
```

Os dados são escritos à medida que chegam; não é guardada em RAM uma cópia da
captura inteira. Um segundo ocupa aproximadamente 122 MiB.

## Segurança

Esta é uma aquisição ativa: usa DMA, IRQ, aquisição e software trigger. O
programa recusa começar se detetar stream, aquisição ou DMA ativos. Uma IRQ já
ativa, por si só, é aceite e preservada. Deve
ser executado apenas numa janela dedicada, sem MARTe ou outro processo a usar a
placa. Não altera chopper, offsets nem a taxa do ADC. Na saída desativa apenas
os mecanismos que iniciou.

`Ctrl+C` tenta terminar de forma limpa. O binário parcial é mantido e
`complete` fica `false` nos metadados.

## Inspeção local

```bash
python3 inspect_capture.py dados.bin
```

Mostra tamanho, número de amostras e estatísticas por canal, sem dependências
externas de Python.
