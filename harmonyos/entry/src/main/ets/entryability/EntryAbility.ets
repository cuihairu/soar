import UIAbility from '@ohos.app.ability.UIAbility';
import type Want from '@ohos.app.ability.Want';
import type AbilityConstant from '@ohos.app.ability.AbilityConstant';
import window from '@ohos.window';

/**
 * Soar entry ability. Loads the single playback page; the actual player
 * wiring (system AVPlayer vs. native soar_core — see harmonyos/README.md)
 * lands in a later batch, so lifecycle hooks stay minimal on purpose.
 */
export default class EntryAbility extends UIAbility {
  onCreate(want: Want, launchParam: AbilityConstant.LaunchParam): void {
    // No media-source handoff yet: file association / open-with arrives
    // together with the player wiring (want.uri consumption).
  }

  onDestroy(): void {
  }

  onWindowStageCreate(windowStage: window.WindowStage): void {
    windowStage.loadContent('pages/Index', (err) => {
      if (err.code !== 0) {
        console.error(`[soar] loadContent failed: ${JSON.stringify(err)}`);
      }
    });
  }

  onWindowStageDestroy(): void {
  }

  onForeground(): void {
  }

  onBackground(): void {
  }
}
