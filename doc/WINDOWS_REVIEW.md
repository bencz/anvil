# Revisão de fontes e validação Windows

Data: 2026-09-04. Ambiente: Windows x64; checkout `E:\programming\anvil`.

O ANVIL e o MCC agora compilam e executam a suíte descrita abaixo nativamente
no Windows. Isso **não equivale a conformidade completa da ABI C**, nem a um
porte completo do Smalltalk. Há defeitos reproduzidos que continuam abertos.

Esta auditoria combina leitura dos caminhos de implementação, compilação dos
fontes, regressões, execução de assembly gerado e testes de interoperabilidade.
Abrange todas as camadas abaixo; não representa uma prova formal nem uma
declaração de inspeção linha a linha de cada arquivo do repositório.

## Arquitetura encontrada nos fontes

| Camada | Implementação e observações |
| --- | --- |
| API e core | `src/core`: contextos, seleção transacional de alvo, DataLayout, tipos, módulos, funções, valores e builders. Tipos/IR congelam configurações dependentes do alvo. |
| Verificação | `src/core/verify.c`: ownership, tipos, CFG, dominância e contratos de operações/chamadas. A verificação precede a geração e acompanha os passes. |
| Otimizador | `src/opt`: propagação de cópias, folding, CSE, simplificação de CFG, DCE, strength reduction e otimizações locais de memória. Pipeline com ponto fixo limitado. |
| MIR | `src/machine`: registradores virtuais, restrições de registradores fixos, blocos, instruções, frame slots, liveness, linear scan, coalescing e materialização de spills. |
| Backends | x86, x86-64, ARM64, PPC e mainframes usam MIR. Descritores e vtables separam seleção, ABI e emissão. PPC e mainframe compartilham implementações entre variantes. |
| MCC | Pré-processador, lexer, parser, semântica, tipos e codegen separados; arena de alocação; headers C próprios. A ABI C do frontend exige políticas adicionais à ABI escalar do backend. |
| Smalltalk | Frontend, grafo de classes, imagem, lowering, roots/safepoints, runtime, GC, primitivas, materialização de artefatos e produto AOT. Os serviços de arquivos/processos do produto ainda pressupõem POSIX. |
| Build/testes | Makefiles e scripts históricos pressupõem shell Unix. O novo runner Windows usa diretamente Clang, o linker nativo e executáveis PE. |

O README estava desatualizado sobre o uso de MIR e a sintaxe de assembly. A API
atual expõe GAS e HLASM; a validação Windows usa GAS/AT&T com diretivas COFF/SEH.

## Ferramentas efetivamente usadas

- Clang/LLVM 18.1.8 em `C:/llvm/bin`, alvo `x86_64-pc-windows-msvc`.
- Bibliotecas MSVC 14.51 e Windows SDK 10.0.26100, encontrados pelo driver Clang.
- Python 3.9.13 para orquestração; `llvm-ar` para os arquivos de objetos.
- Debian/WSL para verificação adicional Linux, registrada separadamente.

O assembler integrado do Clang aceita a saída GAS do ANVIL e produz COFF. O
driver faz o link com o runtime nativo. Não foi necessário instalar MASM, NASM,
FASM, MinGW ou Wine. O GCC 6.3 disponível na máquina era de 32 bits e não foi
usado como referência da ABI Win64.

O link dos samples usa `-Xlinker legacy_stdio_definitions.lib` para funções
stdio. O header especializado Windows obtém stdin/stdout/stderr por
`__acrt_iob_func`, selecionado pelo descritor de includes do alvo. Os headers
continuam simplificados; não substituem a UCRT completa.

## Resultado Windows do levantamento inicial

| Verificação | Resultado |
| --- | --- |
| Escopo ANVIL + MCC + exemplos | 170 registros de build/teste; zero falhas |
| Compilação completa, incluindo Smalltalk | 136 unidades de tradução tentadas; 132 compilaram, 4 falharam |
| Testes C executados no levantamento completo | 25 executáveis aprovados: 12 ANVIL, 2 MCC, 11 Smalltalk |
| Sintaxe e rejeições do MCC | 66 casos aprovados; rejeição exige exit code 1, crash não conta como sucesso |
| MCC versus Clang | 65 programas × `-O0`, `-Og`, `-O1`, `-O2`, `-O3`: 325 comparações aprovadas |
| Assembly gerado | Suítes Win64 ABI e FCMP/i1 executadas e aprovadas |
| Exemplos executados | `basic_runtime`, `fp_math_lib`, `dynamic_array`, `base64_lib`: aprovados |
| AddressSanitizer Windows | 12 executáveis de regressão ANVIL aprovados com a biblioteca instrumentada |
| Smalltalk restante | 41 testes falham ao compilar/linkar; não foram contabilizados como executados |

O levantamento completo registra **275 etapas, 45 falhas**: 4 fontes de produção
Smalltalk e 41 builds de testes Smalltalk. Esses números incluem compilação e
link, não são uma contagem de 275 testes executados.

Logs/manifests locais, preservados em diretório ignorado pelo Git:

- `build/windows/results.json`: levantamento completo.
- `build/windows/results-compiler.json`: escopo ANVIL/MCC/exemplos.
- `build/windows/mcc-exec/results.json`: matriz diferencial.
- `build/windows/asan/results.json`: regressões instrumentadas.
- `build/windows/linux-results/`: resultados Linux individuais.

Algumas regressões fazem cross-assembly ARM64/PPC/i386. Isso valida instruções e
diretivas aceitas pelo assembler; **não executa esses alvos no Windows**. Os
subtestes de execução i386 SysV são explicitamente pulados no host Windows.
Não foi validado Windows ARM64 nem Windows x86 de 32 bits.

## Verificação adicional Linux

No Debian/WSL, o build isolado `build/wsl` compilou as 60 regressões do Makefile
principal. Executadas individualmente, 57 passaram; os resultados restantes foram:

- `smalltalk_dnu_lower_regression`: falha de descriptor mismatch no bootstrap.
  A mesma falha foi reproduzida numa extração intacta de `git archive HEAD` em
  `build/wsl-baseline`, sem as alterações desta revisão.
- `smalltalk_application_aot_regression` e
  `smalltalk_application_samples_regression`: timeout de 180 segundos por teste.
  A causa desses dois timeouts não foi determinada; não são resultados aprovados
  nem foram classificados como defeitos preexistentes.

O MCC também compilou no Linux e passou seus dois testes unitários após a
separação dos serviços de plataforma. A matriz de 325 execuções deste relatório
é Windows; não foi reutilizada como evidência de execução Linux.

Durante o build foi removida uma dependência indevida de `block_primitives.c` do
teste de primitivas de string no Makefile principal. Ela introduzia referências
de closure/AOT que aquele teste não usa. Também foram atualizados os harnesses
Linux que compilam o runtime diretamente para incluir sua implementação POSIX.

Comando do build Linux isolado, executado na raiz dentro do Debian:

```sh
make -j4 HOST_PLATFORM=posix BUILD_DIR=build/wsl LIB_DIR=build/wsl/lib LDFLAGS=-L./build/wsl/lib tests
```

Esse alvo interrompe na primeira falha. O resultado 57/60 veio da execução
individual posterior dos 60 binários, preservada em `build/windows/linux-results`.

## Correções implementadas

### Serviços do host e organização

`src/platform/{posix,windows}/registry.c` implementa a vtable de inicialização
única e exclusão do registro de backends. O core não inclui APIs Windows nem
contém seleção de sistema por preprocessor.

O MCC usa `mcc_host` em `src/platform/{posix,windows}` para alvo padrão e
separadores de caminhos. O runtime Smalltalk usa `st_runtime_platform` para
mutexes e alocação/liberação alinhadas. Os testes de assembly também usam uma
vtable do host. O build seleciona a implementação; não há inclusão de arquivos
`.c` nem implementação Windows espalhada nos consumidores genéricos.

Políticas do **alvo gerado** ficam em descritores: o modelo C do MCC centraliza
ABI, `long`, `size_t`, `ptrdiff_t`, `wchar_t` e macros predefinidas. Os headers
`stddef.h`/`stdint.h` consomem tipos predefinidos pelo compilador.

### MCC nativo

- Removido include de `getopt.h` não utilizado; comparação dos nomes de padrões
  C usa código C portável, sem alias global de função POSIX.
- Renomeado o membro AST `static_assert`, que colidia com macro dos headers CRT.
- Adicionado alvo `x86_64_windows`/`win64`, com ABI Win64 e LLP64.
- Corrigidos tipos de `sizeof`, `_Alignof` e diferença de ponteiros para LLP64.
- Includes abertos em modo binário: `ftell` e `fread` passam a medir os mesmos
  bytes em arquivos CRLF. Busca relativa reconhece caminhos Windows pela vtable.
- Corrigido dereference de tipo nulo após rejeição sintática. O teste negativo
  `c89_bitfields` antes emitia diagnóstico e depois encerrava por access violation.

### Backend x86-64

1. **Buffer dinâmico corrompido em chamada.** `alloca_dyn` devolvia uma região que
   se sobrepunha à área de argumentos de saída. O quinto argumento de uma chamada
   trocava o valor 123 por 999. A alocação agora preserva a área de saída abaixo
   do buffer. A correção também se aplica ao caminho x86-64 SysV.
2. **Sondagem da pilha Windows.** Frames grandes e alocações dinâmicas agora
   tocam cada página antes de mover RSP. O teste executa alocações fixas e
   dinâmicas de 128 KiB numa thread nova.
3. **Unwind com frame dinâmico.** Win64 usa RBP na base do frame fixo,
   `.seh_setframe` e epílogo reconhecível pelo unwinder. Os deslocamentos de
   slots, saves/restores e argumentos recebidos foram ajustados juntos.
   Uma exceção Windows atravessa um frame gerado com `alloca_dyn` e é capturada
   por `__except` no caller nativo.
4. **Operações FP destrutivas.** Quando o resultado recebia o registrador do
   operando direito, a cópia do esquerdo destruía o direito. Agora a emissão
   utiliza temporário volátil reservado nesse caso, preservando a ordem dos
   operandos. O teste de argumentos mistos revelou o problema.
5. **Literais FP.** Labels de máscaras de `fneg`/`fabs` agora incluem a função;
   múltiplas funções no mesmo módulo deixam de redefinir o mesmo label. No
   Windows os literais usam `.rdata`.

Também foram testados argumentos inteiros/FP intercalados, argumentos na pilha,
caller variádico para callee nativo, chamada indireta e preservação dos 128 bits
de XMM6. Isso não cobre todas as combinações possíveis de registradores/unwind.

As regras usadas para o frame, sondagem, epílogo e parâmetros seguem a
[documentação Microsoft de prólogos/epílogos](https://learn.microsoft.com/en-us/cpp/build/prolog-and-epilog),
[convenção x64](https://learn.microsoft.com/en-us/cpp/build/x64-calling-convention)
e [exception handling x64](https://learn.microsoft.com/en-us/cpp/build/exception-handling-x64).

## Defeitos importantes ainda abertos

### Lacunas do levantamento corrigidas na implementação seguinte

A classificação C de agregados Win64 agora pertence à vtable do backend.
O MCC compartilha o plano entre assinaturas, chamadas, definições, retornos
e ponteiros de função: objetos de 1/2/4/8 bytes usam transporte inteiro;
os demais usam cópia do caller alinhada a 16 bytes, com ponteiro oculto no
retorno. A matriz cruza caller/callee MCC e Clang em ambos os sentidos.

O callee variádico Win64 usa `va_start` em IR/MIR e salva os registradores
de entrada nos slots da ABI antes de executar o corpo. `va_arg` usa o
serviço do backend; os testes incluem FP, agregados, `va_copy`, parâmetros
nomeados na pilha, retorno indireto e VLA. Outros formatos de `va_list`
não foram implementados.

`volatile` e alinhamento são contratos explícitos de memória propagados
até o MIR. Os passes preservam os acessos observáveis. Também há contratos
de efeitos de chamadas e uma regressão SEH para stores anteriores a traps.
Isso não implementa atomicidade.

O estado completo e os limites das otimizações estão em
[OPTIMIZER_IMPLEMENTATION.md](OPTIMIZER_IMPLEMENTATION.md). A classificação
Win64 não equivale a legalização geral de valores agregados no MIR nem a
conformidade C de todos os alvos.

### P1 — Produto Smalltalk ainda dependente de POSIX

Fontes que não compilaram no Windows:

- `frontend/source_bundle.c`;
- `compiler/artifact_materialize.c`;
- `compiler/application_materialize.c`;
- `product/aot_toolchain.c`.

Esses caminhos dependem de operações relativas a diretórios, proteção contra
symlinks, publicação atômica, sincronização de arquivos e lançamento/espera de
processos. O produto AOT também restringe o perfil a x86-64 SysV/GAS.

O próximo porte precisa de vtables de filesystem/processo com implementação
Windows baseada em handles e regras explícitas para reparse points, publicação,
substituição e durabilidade. Stubs de `openat`, macros de compatibilidade ou
remover verificações de segurança fariam os testes perderem seu significado.
O arquivo Smalltalk produzido pelo runner completo é **parcial**, destinado
aos testes linkáveis; não é uma biblioteca de produto Windows completa.

### P2 — Headers e identidade de arquivos

Os headers C do MCC ainda têm constantes presumindo 64 bits (`stdint.h`) e
outros contratos simplificados. Corrigir o tamanho do ponteiro não certifica a
libc inteira para cada alvo. `#pragma once` também deve evoluir de comparação de
nomes para identidade/canonicalização de arquivos, incluindo aliases e diferenças
de caixa no Windows. A correção de separadores desta revisão não resolve isso.

### P2 — Alcance real dos testes

Scripts históricos podem tratar ausência de toolchain como sucesso e alguns
testes negativos aceitavam qualquer código não zero. O runner Windows distingue
build, link, execução, timeout e rejeição esperada. É necessário aplicar o mesmo
rigor aos demais runners e consolidar as listas duplicadas de fontes Smalltalk.

## MIR/IR e caminho para melhorar código gerado

Ainda não há evidência que permita afirmar paridade ou superioridade ao LLVM.
Os 325 testes diferenciais verificam comportamento, não desempenho.

| Prioridade | Mudança proposta | Motivo / critério de validação |
| --- | --- | --- |
| 1 | Contratos completos de ABI e memória | Fechar agregados, varargs, volatile e validar nos dois sentidos com Clang; incluir retornos, callbacks, spills e exceções. |
| 2 | Restrições explícitas de instrução no MIR | Modelar operandos vinculados, clobbers, temporários, registradores parciais e restrições de chamada; evitar que regras cruciais existam apenas no emissor. |
| 3 | Liveness por bitsets e worklist | `regalloc.c` mantém quatro matrizes de bytes bloco × vreg e realiza varreduras repetidas. Precalcular use/def e sucessores reduz memória e trabalho. |
| 4 | Intervalos com buracos e splitting | O linear scan atual usa intervalos conservadores; splits, rematerialização e reutilização de spill slots podem reduzir pressão e tráfego de pilha. |
| 5 | Alocação conforme clobbers de chamadas | x64 SysV aloca apenas 4 GPRs e nenhum FPR; Win64 usa 6 GPRs e 7 FPRs preservados. Valores que não atravessam chamadas deveriam poder usar registradores voláteis. |
| 6 | Mem2reg/SROA e análise de alias | O lowering C materializa muitos locals em memória. Promover valores e decompor agregados abre oportunidades para otimizações globais. |
| 7 | GVN/SCCP, loops e inlining | Expandir além de CSE/propagação locais; introduzir análises de loop, LICM, indução e inlining com orçamento/custo. |
| 8 | Custo por alvo e SIMD | Seleção de instruções, addressing modes, scheduling e vetorização dependem de legalização e custos reais, não apenas de habilitar features de CPU. |
| 9 | Métricas e corpus | Medir tempo de compilação, RSS, tamanho de `.text`, loads/stores, spills e tempo de execução com entradas verificadas; comparar mesma ABI/CPU/semântica FP. |

Custos encontrados no levantamento incluíam ordenação por inserção, buscas
lineares, spills por vreg e varreduras por bloco. A implementação seguinte
adicionou heapsort, análises compactas, reutilização de slots e rematerialização;
os demais custos exigem medição em programas representativos.

`-O3` passou a habilitar LICM além do conjunto de `-O2`. Há estatísticas
opcionais por passe/função; ainda não foi estabelecida paridade de desempenho
com LLVM.
As verificações de IR e os contratos de ownership já existentes são uma boa base
para evoluir o pipeline sem transformar miscompilations em ganhos aparentes.

## Validação após as implementações de otimização e ABI

O runner de compilador passou 197 etapas no Windows, incluindo as duas
matrizes de interoperabilidade (25 etapas cada). Foram aprovadas 18 regressões
ANVIL com AddressSanitizer e 512 execuções do corpus gerado. Essas contagens
incluem etapas de build; não são 197 executáveis distintos. Os resultados
Smalltalk do levantamento inicial não foram substituídos por esses números.

A matriz MCC/Clang Windows passou 350 comparações em 70 fontes, cada uma em
O0, Og, O1, O2 e O3: `build/windows/redecl-mcc.log` e
`build/windows/mcc-exec/results.json`.

No Debian/WSL, a matriz adicional passou 350 comparações MCC/Clang: 70 fontes
em O0, Og, O1, O2 e O3. Essa execução revelou a emissão de definições zeradas
para declarações `extern` dos headers C e uma regressão no transporte privado
de retornos agregados dos alvos não Windows. Ambas foram corrigidas. As
declarações/definições de variáveis globais também receberam verificações de
compatibilidade, linkage e inicializadores duplicados. Logs da matriz Linux:
`build/windows/redecl-linux-mcc.log` e `build/wsl/mcc-exec-review/results.json`.

## Ampliação de atomics, SIMD, loops, splitting e ABIs

Atualização em 2026-09-05. Os números anteriores preservam os marcos da
auditoria; o estado atual do compilador foi validado novamente:

| Verificação | Resultado |
| --- | --- |
| Runner Windows ANVIL/MCC/exemplos | 214 etapas, zero falhas |
| MCC versus Clang Windows | 385 comparações, 77 fontes em cinco níveis |
| MCC versus Clang Linux x64 | 385 comparações, zero diferenças |
| AArch64 Linux em QEMU | 1291 etapas, incluindo 385 comparações MCC/Clang |
| Regressões ANVIL com ASan Windows | 23 executáveis, zero falhas |
| Corpus gerado do otimizador | 512 execuções diferenciais, zero diferenças |

Atomics x64/ARM64 incluem load/store, seis RMWs, CAS forte e fences, com
ordens verificadas no IR/MIR. Os runtimes O0/O3 exercitam concorrência,
publicação release/acquire, preservação dos bytes adjacentes e pressão de
registradores. SIMD x64 inclui operações FP128, spills de largura completa e
SLP com prova de limites/alias. O MCC usa `-ffp-vectorize` explicitamente para
permitir alteração na ordem de observação de exceções FP.

Inlining e unrolling O3 têm orçamentos, preservam metadados de memória e
atômicos, reparam PHIs e preparam alocações antes de alterar o CFG. O splitting
divide valores locais spilled nos clobbers, salvando uma vez e recarregando
por segmento. Seus limites estão em `OPTIMIZER_IMPLEMENTATION.md`.

O suporte variádico agora usa armazenamento e layout nativo de `va_list` em
x64 SysV e AAPCS64, além do cursor Win64. Os testes cruzam caller/callee entre
MCC e Clang e verificam cópias independentes em loops, arrays, structs e
passagem por referência. A validação ARM64 descobriu e cobriu a normalização
da ABI default e uma emissão de MOV com larguras incompatíveis.

| ABI | Evidência executável atual | Lacunas relevantes |
| --- | --- | --- |
| Win64 x64 | Windows nativo; escalares, agregados e varargs testados com Clang nos dois sentidos | Não cobre todos os tipos C, extensões SIMD ou todas as combinações de unwind |
| SysV x64 | Linux nativo; corpus e varargs escalares com Clang | Agregados C de classes mistas/múltiplos registradores e varargs agregados |
| AAPCS64 Linux | Corpus, atomics e varargs escalares em QEMU | Agregados/HFA/HVA e testes de ordenação em hardware ARM |
| Darwin ARM64 | Lowering e cross-assembly das regressões existentes | Sem execução macOS nesta revisão; va_start/va_arg e agregados ainda incompletos |
| x86, PPC e mainframes | Regressões de IR/MIR e emissão; parte dos alvos tem cross-assembly | Falta interoperabilidade C abrangente e execução nas plataformas correspondentes |

Isso amplia a cobertura; não conclui a conformidade de todas as ABIs. Os
resultados Smalltalk do levantamento continuam separados e não foram
substituídos pelos resultados do compilador.

Logs finais: `build/windows/final-native-abi-*.log`; manifests Windows nos
diretórios anteriores, Linux em `build/wsl/mcc-exec-review/results.json` e
AArch64 em `build/cross/arm64/results.json`.

## Como reproduzir

Na raiz, em PowerShell:

```powershell
py -3 tests/run_windows.py --cc C:/llvm/bin/clang.exe --scope compiler
py -3 tests/run_windows_mcc.py --cc C:/llvm/bin/clang.exe
py -3 tests/run_windows_sanitize.py --cc C:/llvm/bin/clang.exe
py -3 tests/run_windows.py --cc C:/llvm/bin/clang.exe --scope all
```

O último comando retorna 1 enquanto as falhas Smalltalk registradas persistirem.

```powershell
./build/windows/mcc.exe -arch=win64 -std=c99 -O2 -Isamples/mcc/includes -o build/windows/aggregate_gap.s tests/windows_gaps/aggregate_caller.c
C:/llvm/bin/clang.exe build/windows/aggregate_gap.s tests/windows_gaps/aggregate_native.c -o build/windows/aggregate_gap.exe
./build/windows/aggregate_gap.exe

./build/windows/mcc.exe -arch=win64 -std=c99 -O0 -Isamples/mcc/includes -o build/windows/variadic_gap.s tests/windows_gaps/variadic_callee.c
C:/llvm/bin/clang.exe build/windows/variadic_gap.s -Xlinker legacy_stdio_definitions.lib -o build/windows/variadic_gap.exe
./build/windows/variadic_gap.exe

./build/windows/mcc.exe -arch=win64 -std=c99 -O2 -Isamples/mcc/includes -o build/windows/volatile_gap.s tests/windows_gaps/volatile_store.c
```

Esses arquivos preservam os casos mínimos encontrados no levantamento. As
correções são cobertas pelas matrizes `tests/run_windows_abi.py` (agregados
 e varargs), além das regressões de contratos de memória.
