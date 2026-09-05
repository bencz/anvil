# Evolução do ANVIL: implementação e validação

Este documento acompanha o pedido de implementar a evolução do núcleo ANVIL,
preservando C, assembly textual, descritores de alvo e vtables. Uma etapa só
é marcada como concluída depois de implementação e testes; APIs ou estruturas
isoladas não contam como uma otimização implementada.

## Sequência e critérios

- [x] Contratos de memória e efeitos: volatile, alinhamento, efeitos de chamadas;
      propagação frontend → IR → MIR e testes contra otimizações indevidas.
- [ ] Contratos aritméticos/FP explícitos e operações atômicas legalizadas por alvo.
- [x] Análises compartilhadas: CFG, dominância, usos, loops, alias e invalidação.
- [x] Promoção de allocas escalares para SSA (mem2reg), incluindo junções e loops.
- [x] Decomposição segura de agregados locais (SROA).
- [ ] SCCP, GVN, known bits/intervalos e otimizações globais de memória.
- [ ] Restrições de instrução/clobbers explícitos no MIR e verificação.
- [x] Liveness com bitsets/worklist e ordenação eficiente de intervalos.
- [ ] Splitting, registradores voláteis conforme chamadas, rematerialização e slots reutilizáveis.
- [ ] Passes de máquina: cópias, endereçamento, branches, frames e scheduling.
- [x] Mecanismo compartilhado de cópias paralelas/eliminação de PHIs.
- [ ] Planos de ABI para chamadas/retornos/agregados/varargs, usados por caller e callee.
- [ ] LICM, indução, loops mortos, unrolling e inlining com custo/orçamento.
- [ ] Legalização/modelo de custo SIMD e vetorização.
- [ ] Estatísticas por passe/função, corpus diferencial e redução de falhas.

## Validação

Executar regressões específicas a cada alteração, depois as suites Windows e
Linux apropriadas. Manter os defeitos preexistentes do levantamento Windows
separados das regressões introduzidas. Preservar as alterações anteriores do
workspace. Não substituir contratos ausentes por stubs que silenciem erros.

## Implementações presentes

| Área | Implementação | Limites atuais |
| --- | --- | --- |
| Memória | `load_ex`/`store_ex`, volatile e alinhamento; qualificação MCC e propagação para os cinco lowerings MIR | Alinhamento explícito deve ser pelo menos o natural; não equivale a atomicidade |
| Chamadas | Máscara explícita de leitura, escrita, captura, exceções, término e efeitos observáveis, consumida por DCE e passes de memória | Sem anotação, o contrato continua totalmente conservador; chamadas indiretas não herdam atributos de um destino presumido |
| CFG | Adjacências compactas, RPO iterativa, dominadores imediatos, cache com comparação exata de arestas e snapshots com referências | Def-use é um snapshot separado, reconstruído após mudanças nos operandos |
| Dominância/loops | Fronteiras de dominância compactas; loops naturais com membros em bitsets, preheader, latches, saídas e aninhamento; LICM usa essa análise | Ciclos irredutíveis não são convertidos em loops com uma entrada; análises derivadas devem ser reconstruídas após mudanças no CFG |
| Alias | Identidade de objetos e intervalos constantes de GEP/STRUCT_GEP; eliminação global de cargas prova preservação em todos os caminhos, incluindo backedges | Endereços desconhecidos, offsets dinâmicos e intervalos fora do objeto continuam conservadores; sem MemorySSA |
| SSA | Mem2reg com inicialização garantida, PHIs em junções/loops e remoção de PHIs triviais | Allocas escalares na entrada; não promove escapes, volatile, alocações dinâmicas ou caminhos sem inicialização |
| Agregados | SROA para endereços constantes, incluindo campos aninhados e elementos de arrays | Rejeita sobreposição, escapes, reinterpretation, acessos agregados e volatile |
| Constantes | SCCP com worklist de usos/arestas executáveis; bits conhecidos para lógica, casts, shifts e soma/subtração; limites com e sem sinal para comparações | Não relaxa semântica FP; sem análise geral de intervalos |
| Redundância | GVN de expressões inteiras dominantes | Sem PRE ou numeração global de memória |
| Loops | LICM inteiro e unrolling completo de loops canônicos com até oito iterações, limite de crescimento e atualização simultânea dos PHIs | Sem unrolling parcial/runtime; LICM não move divisões, cargas, chamadas ou FP |
| Inlining | Clonagem de CFG/PHIs de funções internas leaf, com múltiplos retornos, orçamento e ordem callee-first | Sem recursão, varargs, alloca, entrada com backedge ou mudança de política FP |
| Regalloc | Liveness com bitsets/worklist, heapsort, slots reutilizáveis e consultas aos clobbers; x64 usa pools voláteis; splitting local salva uma vez e recarrega por segmento | Splitting exige definição única e usos no mesmo bloco; sem intervalos globais com buracos ou reutilização dos slots fixados pelo splitting |
| Verificador MIR | Confere por liveness que registradores alocados sobrevivem aos clobbers entre definição e uso, inclusive através de blocos | Ainda não modela operandos amarrados, early clobbers, sobreposição parcial de registradores ou todas as formas legais por alvo |
| Aritmética | Divisão/resto com sinal por potências de dois positivas usam bias e shifts, preservando truncamento para zero; limites com sinal a partir de bits conhecidos | Sem expansão de divisores negativos ou alterações nos contratos FP; ainda não há modelo de custo completo por alvo |
| Spills | Rematerialização de constantes inteiras com uma única definição | Não rematerializa cargas, endereços, FP ou valores com múltiplas definições; slots reservados podem permanecer no frame |
| PHIs | Emissor compartilhado de cópias paralelas, com temporários tipados para ciclos e rollback de falha | Opera antes do regalloc; valida destinos únicos e compatibilidade de classe/largura |
| Medição | Estatísticas opcionais e observer por passe/função, incluindo convergência e tamanho do IR | Tempo é CPU do processo e inclui verificação; não é uma comparação de desempenho com LLVM |
| ABI Win64 | Classificador de agregados na vtable do backend; assinaturas e transporte compartilhados entre chamadas, definições, retornos e ponteiros de função no MCC | Suporte C por coerção escalar/cópia indireta; não legaliza valores agregados arbitrários no MIR nem implementa a ABI C de agregados dos demais alvos |
| Varargs | Win64 inclui agregados; x64 SysV e AAPCS64 têm save areas, esgotamento independente de GPR/FP, pilha e layout nativo de `va_list` no MCC | SysV/AAPCS64 aceitam argumentos variádicos escalares; demais ABIs e agregados variádicos continuam pendentes |
| Atomics | Contratos de ordem, load/store, seis RMWs, CAS forte e fences; emissão x64 e ARM64 para inteiros/ponteiros de 8/16/32/64 bits | Alinhamento natural obrigatório; sem atomics agregados, expansão por biblioteca ou frontend C `_Atomic` |
| SIMD | Tipo vetorial, operações FP e custos na vtable; x64 emite FP128, incluindo spills completos; SLP combina streams contíguos com prova de limites/alias | Opt-in por função para ordem das exceções FP; sem reassociação, SIMD inteiro, shuffle, vetorização geral de loops ou ABI vetorial |
| Corpus | Gerador determinístico de C sem overflow com sinal; comparação com Clang, gravação de seeds e redução por remoção de operações | O redutor ainda não foi exercitado por uma falha real do compilador nesse corpus; não constitui prova de equivalência |

Os novos testes ficam em `tests/optimization_pipeline_regression.c` e
`samples/mcc/tests/exec/ssa_promotion_c99.c`. Eles também revelaram e cobrem
dois defeitos corrigidos: labels `case` consecutivos no MCC e dependência da
ordem física dos blocos na geração MIR. A travessia RPO é compartilhada pelos
cinco lowerings, preservando a seleção especializada de plataforma/alvo.

Os contratos de chamadas também são propagados até o MIR; chamadas sem
anotação e chamadas indiretas permanecem conservadoras. Chamadas `void` sem
efeitos podem ser removidas pelo DCE. O teste nativo SEH executa uma divisão
por zero e verifica que o store anterior permanece observável em O3.

A validação Linux também identificou emissão incorreta de definições para
variáveis `extern` do MCC, incluindo `stdout` dos headers. O cache agora cria
declarações e só define armazenamento quando existe uma definição na fonte.
Declarações compatíveis e definições tentativas compartilham o símbolo; tipos,
linkage conflitantes e inicializadores duplicados continuam sendo rejeitados.
Foi preservado o transporte anterior de retornos agregados dos demais alvos;
isso não atribui conformidade de ABI nativa à convenção privada antiga.

Marcos de validação: 214 etapas do runner Windows sem falhas, incluindo as
matrizes Win64 de agregados/varargs e os runtimes de atomics e SIMD em O0/O3;
385 comparações MCC/Clang no Windows e outras 385 no Debian/WSL, sem diferenças.
As regressões ANVIL passaram em 23 executáveis com ASan. O corpus gerado
passou 512 execuções após as alterações de alocação e memória, registradas
em `build/windows/optimizer-fuzz`. A matriz AArch64 em QEMU passou 59 etapas,
incluindo atomics em O0/O3 e interoperabilidade variádica MCC/Clang em cinco
níveis, nas direções caller, callee e ambos gerados pelo MCC.
Com o corpus completo, o runner AArch64 registra 1291 etapas, incluindo 385
comparações de execução. São 77 fontes em cinco níveis de otimização.
As regressões de otimização, análise e classificação de ABI passaram também
no Debian/WSL. Contagens de etapas incluem compilações e não equivalem ao
número de programas executados. As etapas não marcadas acima continuam
abertas; este documento não representa conclusão de todo o roadmap.

## Contratos e reprodução das novas implementações

O unroller limita o corpo a 32 instruções e o crescimento a 128 instruções.
A simulação da indução respeita a largura inteira, incluindo wrap sem sinal,
zero iterações e passos negativos. O inliner usa custo máximo 48 no callee e
orçamento 1024 no caller; prepara as alocações antes de alterar o CFG vivo.
As regressões incluem falhas de alocação, switches e PHIs cíclicos. Elas também
cobrem geração para as dez arquiteturas, sem atribuir execução nativa a todas.

O splitting seleciona valores inicialmente spilled que atravessam clobbers.
Reserva um slot imutável, cria novos vregs para os segmentos e repete a
alocação. Valores entre blocos e múltiplas definições permanecem conservadores.
Os testes exercitam GPR, FPR e materialização de spills.

O runtime SIMD confere 1024 casos por nível, incluindo NaN, infinito, zero com
sinal, endereços sem alinhamento vetorial e clobber de todos os XMM voláteis.
O sample `simd_packing.c`, com `-O3 -ffp-vectorize`, gera `mulps` e `addpd` e
mantém o resultado 1105.0000 no Windows e Linux.

Os runtimes atômicos conferem 164 combinações por nível, 400 mil atualizações
concorrentes com RMW/CAS, 20 mil publicações release/acquire e CAS com 32 valores
vivos para forçar spills de operandos/resultados. Em x64 usam MOV,
XCHG, LOCK XADD/CMPXCHG e MFENCE; em ARM64 usam LDR/STR, LDAR/STLR,
LDXR/STXR ou LDAXR/STLXR, CLREX e DMB. Execução emulada não substitui
validação de litmus tests de memória fraca em hardware ARM.

Referências dos contratos: [Intel SDM](https://www.intel.com/content/www/us/en/developer/articles/technical/intel-sdm.html),
[ordenação Armv8](https://developer.arm.com/community/arm-community-blogs/b/tools-software-ides-blog/posts/armv8-sequential-consistency),
[AAPCS64](https://github.com/ARM-software/abi-aa/blob/main/aapcs64/aapcs64.rst) e
[ABI x64 SysV](https://gitlab.com/x86-psABIs/x86-64-ABI/-/raw/master/x86-64-ABI/low-level-sys-info.tex).

O runner `tests/run_arm64.py` usa Clang/LLD, sysroot AArch64 e QEMU sem registrar
binfmt ou modificar o sistema. Neste workspace, pacotes Debian foram baixados
e extraídos em `build/cross/root`. Dentro do Debian/WSL, após compilar a
biblioteca e o MCC para o host:

```sh
python3 tests/run_arm64.py --cc clang-19 --lib build/wsl/lib/libanvil.a --mcc build/wsl/mcc --sysroot build/cross/root/usr/aarch64-linux-gnu --qemu build/cross/root/usr/bin/qemu-aarch64
```

`--corpus` acrescenta a comparação do corpus MCC completo contra Clang, ambos
executados sob QEMU. O runner grava logs e `results.json` em `build/cross/arm64`.

Os headers especializados do MCC agora definem o `va_list` nativo: array de
uma struct de 24 bytes no SysV x64 e struct de 32 bytes no AAPCS64. O serviço
`anvil_build_va_copy_into` copia o estado para armazenamento do caller, evitando
compartilhamento acidental quando um mesmo `va_copy` executa repetidamente.
As matrizes de interoperabilidade conferem tamanho, padding/alinhamento em
structs, cópias em arrays e consumo por referência entre MCC e Clang. A
regressão `variadic_copy_array.c` reproduzia 334455 em vez de 112233 antes
dessa correção. O ajuste de parâmetros por typedef de array é aplicado na
construção do tipo da função, inclusive no parser.

Continuam abertas a cobertura C completa de agregados das demais ABIs,
varargs Darwin/PPC/x86/mainframe, SIMD fora de x64 e atomics fora de x64/ARM64.
As implementações acima não demonstram paridade de desempenho com LLVM.
