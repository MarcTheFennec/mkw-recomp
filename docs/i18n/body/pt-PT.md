Uma recompilação estática de *Mario Kart World* (Nintendo Switch 2, versões 1.0.0 e 1.8.0) num programa nativo de
PC para Linux e Windows. Precisa da sua própria cópia do jogo, extraída da sua própria consola: nada do jogo está
neste repositório nem nas suas versões publicadas.

**É uma recompilação, não um emulador.** O código AArch64 do jogo é traduzido para C antecipadamente e compilado para
o CPU do PC (x86-64, ou AArch64 em Linux); não há interpretador nem JIT de CPU. A GPU também não é emulada: o NVN2, a
API gráfica da Switch 2, é reimplementado sobre Vulkan ao nível da API, e os shaders SM86 do jogo são traduzidos para
SPIR-V antecipadamente. As funções do SDK da Nintendo (`nn::*`) são substituídas pelo nome por código próprio.

## Estado

Numa fase inicial, e a avançar depressa. A 12/11/2026:

- Arranca, reproduz os vídeos de introdução, percorre os menus e corre corridas, em Linux e em Windows (x86-64), e em
  Linux sobre AArch64 (com o seu próprio AppImage; testado numa consola portátil com Adreno 740).
- O ecrã inicial prepara o jogo a partir da sua cópia na sua máquina (gera e compila o C, traduz os shaders, aquece a
  cache de pipelines) e guarda todas as definições: ecrã, qualidade gráfica, controlos, hacks.
- As corridas correm a 60 fps em GPUs topo de gama e a 30 nas de gama média; veja [Desempenho](#performance).
- Problemas conhecidos: alguns menus (modo, seleção de personagem e de veículo) ainda precisam de trabalho, a sombra do
  kart perde um fotograma de vez em quando, parte do áudio corta-se nos vídeos e com alguns controladores da AMD a GPU
  pode bloquear. Não há jogo online.

## Roteiro

O principal trabalho em aberto, por ordem; os pormenores estão em [docs/roadmap.md](docs/roadmap.md).

- **60 fps nas corridas.** Hoje um fotograma de corrida é uma cadeia em série: o jogo termina o seu fotograma
  (~3 ms\*), depois o back end Vulkan grava o grande envio do jogo (~19 ms\*), depois a GPU executa-o (~10 ms\*), e só
  então começa o fotograma seguinte.
  Gravar está no caminho crítico, por isso o trabalho é gravar com menos CPU: estado dinâmico só quando muda, caches de
  desenhos entre fotogramas, gravação em paralelo e uma thread do jogo mais rápida.
- **A imagem como na consola**: os últimos erros do tradutor de shaders, a sombra do kart, push constants, zero erros
  de validação do Vulkan.
- **Todos os menus verificados** com a consola: os próximos são modo e seleção de personagem e de veículo.
- **Sem bloqueios de GPU** e menos memória de GPU; depois, carregamentos sem soluços e um pacote para distribuições
  Linux que não são rolling release.
- **A 1.8.0 ao nível da 1.0.0**: o seu formato ultrawide, o tamanho do mapa de sombras e a semente fixa
  ([#19](https://github.com/adriadam10/mkw-recomp/issues/19)).

\* Medido a 09/11/2026 na nossa máquina de testes: APU Radeon 660M (Ryzen 5 7535HS), Linux, resolução interna 720p.

<a id="performance"></a>

## Desempenho

Os tempos por fotograma medidos, cada um com a versão testada, estão em [docs/performance.md](docs/performance.md). Para
acrescentar a sua máquina, abra um
[relatório de desempenho](https://github.com/adriadam10/mkw-recomp/issues/new?template=performance.yml): entra na
tabela com a versão indicada. Os tempos são os fotogramas do próprio jogo: a geração de fotogramas mostra mais
fotogramas, mas não os encurta. 16,7 ms são 60 fps; com a Taxa de fotogramas em Automático (a da consola) desce para
30 fps quando não se chega aos 60, e um fotograma acima de 33,3 ms faz a corrida andar mais devagar do que o tempo
real.

| Data | GPU | Resolução interna | fps na corrida | Tempo por fotograma |
|---|---|---|---|---|
| 11/11/2026 | RX 7900 XTX (Windows) | 2160p | 60 | 16,7 ms |
| 11/11/2026 | RX 9070 XT (Linux, RADV) | 720p a 2160p | 60 | 16,7 ms |
| 10/11/2026 | RTX 3060 (Windows) | 1080p | ~48 | ~21 ms |
| 11/11/2026 | RTX 3050 Laptop (Windows) | 720p | ~37 | 26,7 ms |
| 09/11/2026 | Radeon 660M (Linux, RADV) | 720p | 30-32 | 31-33 ms |
| 11/11/2026 | GTX 1650 SUPER (Windows) | 1080p | ~24 | ~41 ms |
| 09/11/2026 | GTX 1050 Ti (Windows) | 1080p | 22-24 | 42-45 ms |

## Como jogar

![O ecrã inicial com o jogo já preparado](docs/images/launcher-main.png)

![A página Ficheiros do jogo, onde a preparação recebe o dump do jogador](docs/images/launcher-files.png)

1. Extraia o dump de Mario Kart World 1.0.0 ou 1.8.0 da sua Switch 2 (`exefs/` e `romfs/`, desencriptados; a 1.8.0 com
   a atualização integrada na RomFS do jogo base), ou esse mesmo dump empacotado num único ficheiro `.nup` ou `.nx`.
2. Transfira o launcher para o seu sistema em [Versões](https://github.com/adriadam10/mkw-recomp/releases): em Linux o
   `.AppImage` (`-aarch64` em ARM; a pasta do jogo por omissão é `~/.local/share/mkw-recomp`; glibc 2.31 ou mais
   recente: Debian 11, Ubuntu 20.04, SteamOS e posteriores), em Windows o `.zip`, descompactado numa pasta. Ambos
   precisam de cerca de 30 GB livres.
3. Execute o AppImage (antes, `chmod +x`) ou o `mkw.exe`, abra **Ficheiros do jogo**, se quiser use **Escolher pasta do
   jogo** para escolher onde o jogo será instalado (ou uma instalação já existente), indique o seu dump (**Escolher
   pasta** ou **Escolher ficheiro .nup ou .nx**) e carregue em **Preparar**. O launcher lembra-se da pasta de
   instalação; é aí que ficam o jogo compilado, os ficheiros preparados, os jogos guardados, as definições e as caches.
   Escolher outra pasta deixa os ficheiros existentes onde estão.
   A primeira preparação demora algum tempo (compila o jogo inteiro e transfere o `nvdisasm` e o `ptxas` da NVIDIA,
   84 MB em Linux e 126 MB em Windows, para descrever a codificação dos shaders na sua máquina); depois, **Jogar**.
4. Quando sai uma versão nova, o ecrã inicial propõe-na (**Atualizar para vX**): transfere-a, verifica-a, substitui o
   programa e volta a preparar só o que mudou. **Definições → Menu → Procurar atualizações** desativa essa verificação.

Requisitos: CPU x86-64 com AVX2 (ou, em Linux, AArch64 com ARMv8.2: LSE, FP16, RCpc, DotProd, CRC32), uma GPU com
Vulkan 1.3 (`VK_EXT_mesh_shader` recomendado; sem ele, os task e mesh shaders correm como compute) e 16 GB de RAM
recomendados. Num contentor Docker ou Podman em Linux, permita `userfaultfd` no perfil seccomp (o do Docker recusa-o
por omissão): sem ele, o port não sabe que memória do jogo mudou e fica várias vezes mais lento.

## Funcionalidades extra

Para além da consola, todas opcionais e desligadas (ou no valor da consola) por omissão:

- **Ecrã**: resolução interna acima ou abaixo da da consola, 21:9, 32:9 e 48:9 (as corridas mostram mais dos lados),
  escalamento AMD FSR 1, HDR em ecrãs HDR, VSync, apresentação mailbox ou imediata.
- **Geração de fotogramas**: fotogramas criados entre os do jogo (uma corrida a 30 fps mostrada a 60, ou a 240 num ecrã
  de 240 Hz). Mais fluido, não mais rápido: a imagem chega até um fotograma mais tarde, e o movimento rápido e o
  minimapa podem ficar esborratados.
- **Gráficos**: filtragem e qualidade de texturas, distância de detalhe e qualidade das sombras acima ou abaixo das da
  consola.
- **Som**: som surround 5.1.
- **Controlos**: todas as teclas e botões reatribuíveis, câmara com o rato, ícones de botões da Nintendo, Xbox ou
  PlayStation, direção por movimento, intensidade da vibração; multijogador local com um comando para cada um.
  Os comandos da Switch 2 (Pro Controller, Joy-Con, GameCube) funcionam por Bluetooth e, em Linux, por USB; em Windows
  o USB precisa de um passo único com o controlador, mais abaixo.
- **Hacks**: turbo (manter ou alternar F2), saltar vídeos, desbloquear tudo, a mesma aleatoriedade sempre, diretamente
  para contrarrelógio ou modo livre.
- **Idiomas**: os menus em 20 idiomas; o próprio jogo também em catalão, galego e basco.
- **Ficheiros do jogo**: escolher a pasta de instalação; um dump em pastas ou num único ficheiro `.nup` ou `.nx`, usado
  onde está; atualizações a partir do ecrã inicial.
- **Sobreposição de desempenho** (FPS, ou CPU, GPU e memória por completo) e informação do sistema num clique para os
  relatórios de erros.

### Comandos da Switch 2 por USB em Windows

Estes comandos só arrancam depois de uma ordem enviada à sua segunda interface USB, que o Windows deixa sem um
controlador que uma aplicação possa usar. Uma vez por comando, com o [Zadig](https://zadig.akeo.ie/):

1. Ligue o comando com um cabo USB-C e abra o Zadig.
2. **Options → List All Devices**, e escolha a **Interface 1** do comando (ID USB `057E` e `2069` para o Pro
   Controller, `2066`/`2067` para o Joy-Con direito/esquerdo, `2073` para o comando de GameCube). Não a Interface 0:
   essa é a entrada normal do comando, e substituí-la esconde-o de todas as outras aplicações.
3. Controlador **WinUSB**, depois **Replace Driver** (ou **Install Driver**).

O mesmo vale para um comando virtual que apareça com esses ID USB (o modo USB/IP do Switch2Connect). Para desfazer,
desinstale o dispositivo dessa interface no Gestor de Dispositivos com «eliminar o controlador» marcado.

## Mods

A página **Mods** do ecrã inicial instala os mods tal como são publicados para o jogo e lista-os em dois separadores:

- **Procurar no GameBanana**: os mods de Mario Kart World no [GameBanana](https://gamebanana.com/games/22152),
  listados ou pesquisados, com o autor, a licença, as capturas de ecrã e a descrição de cada um. **Instalar** transfere
  o ficheiro do GameBanana, só quando o pede.
- **Instalados**: os seus mods, cada um com o seu interruptor; **Importar zip / Importar pasta** recebe um mod que já
  tenha (`.zip`, `.rar` ou `.7z`, com `romfs/` e/ou `exefs/` em qualquer sítio, incluindo a estrutura
  `atmosphere/contents/`).

Os mods que substituem ficheiros do jogo (`romfs/`: skins, texturas, textos, pistas) aplicam-se no arranque seguinte.
Os que mudam o código do jogo (patches `.ips` ou `.pchtxt` de `exefs/`, como 200cc ou 30 FPS) são integrados no código
do jogo: ligar ou desligar um volta a compilá-lo antes de jogar (alguns minutos). Dois mods que mudam o mesmo pacote
são combinados ficheiro a ficheiro, e a página pergunta qual prevalece onde os dois mudam o mesmo ficheiro. A página
avisa sobre mods que substituem as definições gráficas ou os shaders do jogo, que o port trata à sua maneira.

Os mods são feitos por outras pessoas e não são testados por este projeto: um mod pode bloquear o jogo ou danificar o
seu jogo guardado. Leia a página de um mod antes de o instalar, guarde uma cópia do seu jogo guardado e comunique os
problemas de um mod ao seu autor; quando reportar um erro aqui, diga que mods estavam ligados (o registo lista-os), ou
experimente antes sem eles.

## Compilar a partir do código-fonte

Linux, com `uv`, `clang`, `lld`, `ninja`, SDL3, Opus, libvpx, zstd, xxHash, os cabeçalhos do Vulkan e VMA 3.4.0
(pormenores em [tools/build/README.md](tools/build/README.md)). Coloque o seu dump em `game/` (`game/exefs`,
`game/romfs`) ou defina `MKW_GAME=<dir>`, e a partir da raiz do repositório:

```sh
uv run python recomp/gen.py --exefs game/exefs --out build/generated --jobs 8 --patches recomp/patches-mkw.txt  # 1.8.0: patches-mkw-180.txt
uv run tools/shaders/extract.py && uv run tools/shaders/translate.py
uv run tools/build/build.py              # build/mkw; --target windows compila o mkw.exe com llvm-mingw
build/mkw
```

A partir de um dump empacotado (`.nup`, `.nx`) e para Linux em AArch64:
[tools/build/README.md](tools/build/README.md#a-packed-dump) e [a sua secção AArch64](tools/build/README.md#linux-aarch64).

Traduzir os shaders precisa da especificação da codificação SM86, gerada a partir do `nvdisasm` da NVIDIA (não
distribuído): veja [tools/shaders/README.md](tools/shaders/README.md). Tudo o que é gerado vai para `build/`, nunca
para o git.

| Pasta | Conteúdo |
|---|---|
| [recomp/](recomp/README.md) | O gerador: AArch64 → C |
| [runtime/](runtime/README.md) | O que faz de consola: kernel, memória, substitutos do SDK, NVN2 sobre Vulkan, áudio, entrada, menus e launcher |
| [sm86/](sm86/README.md) | O tradutor de shaders: SASS SM86 → GLSL → SPIR-V |
| [i18n/](i18n/README.md) | Os textos dos menus, do ecrã inicial e da preparação em todos os idiomas (gettext `.po`) |
| [tools/](tools/README.md) | Análise, preparação, compilação, sessões de jogo e diagnósticos |
| [tests/](tests/README.md) | Testes sem o jogo nem GPU, no CI em Linux e Windows |
| [docs/](docs/README.md) | Investigação, fontes e desempenho |
| `.github/` | CI, modelos de issues e de pull requests |

Na raiz: `pyproject.toml` e `uv.lock` (o ambiente Python das ferramentas), `.gitignore`, `LICENSE`, `CONTRIBUTING.md`,
`CHANGELOG.md` (as alterações de cada versão, as suas notas) e `AGENTS.md` (um guia para agentes de programação).

## Decisões de conceção (30/10/2026)

| Tema | Decisão | Porquê |
|---|---|---|
| CPU | Tradutor AArch64→C baseado no do [totk-pc-recomp](https://github.com/valentderah/totk-pc-recomp) (MIT), alargado a ARMv8.1-8.3: atómicos LSE, CAS, LDAPR; PAC como identidade, etiquetas MTE a 0 | A mesma família de motor do MKW, e já escala para um executável grande |
| Sistema | As funções `nn::*` do SDK substituídas pelo nome; assinaturas tiradas dos nomes decorados dos 1 289 imports do `main` | Não há serviços da consola a emular |
| Gráficos | NVN2 sobre Vulkan: 529 das suas 699 funções têm o mesmo nome que no NVN1 (assinaturas conhecidas pelo totk-pc-recomp e pelo nxnative); 170 são novas | Recompilação, não emulação da GPU |
| Shaders | Tradutor próprio SM86→SPIR-V, offline, com um descodificador gerado a partir da especificação do DocumentSASS / nvsass-disassembler | Cobre mesh/task e o que falta à Maxwell |
| Funcionalidades opcionais da Switch 2 | HDR ligado sozinho quando o ecrã está em HDR, como na consola (Definições > Ecrã > HDR: Automático ou Não); DLSS, ray tracing e toque atrás de opções, desligados por omissão; desligados, as funções do NVN2 existem mas não fazem nada, para o jogo seguir o caminho normal | Extras do hardware da consola, não fazem falta para jogar |
| Predefinições | O aspeto e a resolução da consola; definições mais baixas são opções | Escolhe o jogador |

## Como foi feito

Este port foi escrito com o [Claude](https://claude.com/claude-code) (o agente de programação da Anthropic) a fazer a
maior parte da escrita, sob a minha direção, em cerca de dez dias de trabalho a tempo inteiro: 489 commits desde
30/10/2026, cada um revisto, medido ou testado antes de entrar. Não foi um prompt e pronto. Por trás disso:

- **Método mais do que prompts.** Cada alteração de desempenho foi medida com um A/B dentro da mesma sessão (duas
  sessões do mesmo build diferem até 12 ms por fotograma); cada correção de imagem foi verificada com a imagem da
  própria consola; as alterações ao tradutor de shaders são testadas fora da GPU em cada um dos 11 561 programas do MKW
  antes de uma sessão; capturas de fotogramas reproduzem o back end em segundos sem o jogo. As regras que saíram dos
  erros estão escritas e são aplicadas por scripts, para o mesmo erro não acontecer duas vezes.
- **O que tinha aprendido antes.** O recompilador e o runtime nasceram do
  [totk-pc-recomp](https://github.com/valentderah/totk-pc-recomp) e do seu port para Linux, e do meu trabalho anterior
  noutros recomps (SavageWindRecomp, smo-recomp): o tradutor, o launcher e a preparação que compila o jogo na máquina
  do jogador, os ícones de botões, o fluxo de controlo dos shaders. Como responsável pelo
  [AnyPS5](https://github.com/boykopovar/AnyPS5) aprendi a traduzir shaders de GPU com fidelidade, a correr mesh
  shaders como compute em GPUs que não os têm e a manter um projeto grande honesto com verificações e medições.
- **A investigação da comunidade.** As notas do [eden-nx2](https://git.eden-emu.dev/Takoroka/eden-nx2) sobre a
  Switch 2, a codificação SASS da NVIDIA tal como documentada pelo DocumentSASS e pelo nvsass-disassembler, o Ryujinx e
  o Mesa NAK para a semântica dos shaders, e os outros ports de Switch 2 listados nas fontes.

## Licenças e créditos

Este projeto está sob a [Licença MIT](LICENSE). O código tirado de outros projetos mantém a sua própria licença, junto
a ele (ficheiros `LICENSE.*` em [recomp/](recomp/), [runtime/](runtime/), [sm86/decode/](sm86/decode/) e
[runtime/third_party/](runtime/third_party/README.md)): totk-pc-recomp, nvsass-disassembler, Dear ImGui, bc7enc,
stb_image, AMD FidelityFX FSR 1 e Roboto. Cada fonte usada, com a sua licença e o que dela foi tirado (código só de
projetos MIT; de projetos GPL, só ideias), está listada em [docs/research.md](docs/research.md#sources).

Mario Kart World é © Nintendo. Este projeto não é afiliado nem apoiado pela Nintendo, não contém código nem recursos do
jogo e não ajuda a obtê-los.

Os mods pertencem aos seus autores e têm as suas próprias licenças. Este projeto não é afiliado nem apoiado pelo
GameBanana nem pelo autor de nenhum mod, não aloja, revê nem distribui mods, e não é responsável pelo que um mod faz:
o ecrã inicial só mostra a listagem pública do GameBanana e transfere um mod do GameBanana quando o pede.

## Contribuir

Veja o [CONTRIBUTING.md](CONTRIBUTING.md). Os erros e os relatórios de desempenho vão para as
[Issues](https://github.com/adriadam10/mkw-recomp/issues), as perguntas para as
[Discussions](https://github.com/adriadam10/mkw-recomp/discussions).
