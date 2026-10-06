/**
 * Soar playback page — UI shell only.
 *
 * Buttons give local state feedback and do NOT pretend to play anything:
 * the kernel (system AVPlayer or native soar_core) is a later batch, see
 * harmonyos/README.md for the two candidate routes. The control surface
 * below mirrors the desktop OSC semantics (transport / volume / rate) so
 * the wiring batch can bind them one-to-one.
 */

interface ControlAction {
  label: string;
  hint: string;
}

@Entry
@Component
struct Index {
  @State status: string = '骨架版 · 播放内核待接线';
  @State lastAction: string = '';
  private actions: ControlAction[] = [
    { label: '播放', hint: 'space 播放/暂停（桌面语义，对齐 OSC）' },
    { label: '暂停', hint: '画面保留，进度停在原位' },
    { label: '停止', hint: '回到空态（Open a file）' },
  ];

  private tap(action: ControlAction): void {
    this.lastAction = `${action.label} —— ${action.hint}`;
  }

  build() {
    Column() {
      Text('Soar')
        .fontSize(30)
        .fontWeight(FontWeight.Bold)
        .margin({ top: 96 })
      Text('万般格式，任其翱翔')
        .fontSize(14)
        .opacity(0.65)
        .margin({ top: 8 })

      Text(this.status)
        .fontSize(13)
        .fontColor('#a2abb8')
        .margin({ top: 24 })

      Column() {
        ForEach(this.actions, (action: ControlAction) => {
          Button(action.label)
            .width(200)
            .margin({ top: 12 })
            .onClick(() => {
              this.tap(action);
            })
        }, (action: ControlAction) => action.label)
      }
      .margin({ top: 32 })

      Text(this.lastAction)
        .fontSize(12)
        .fontColor('#6e7886')
        .margin({ top: 24 })

      Blank()

      Text('桌面播放器的移动端壳；解码内核与格式边界见 docs/mvp.md')
        .fontSize(11)
        .fontColor('#4d5560')
        .margin({ bottom: 32 })
    }
    .width('100%')
    .height('100%')
    .backgroundColor('#0e1013')
  }
}
