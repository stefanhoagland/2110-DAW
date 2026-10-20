// A tiny Web Audio Module (WAM 2.0) for the browser test: a gain plugin built
// on the official SDK, the way third-party plugins are, with its own editor
// (gui.js). Load it as index.js?nogui to get a plugin without an editor.
import { WebAudioModule, WamNode, addFunctionModule } from '../../../js/vendor/wam-sdk.js';

const NO_GUI = new URL(import.meta.url).searchParams.has('nogui');

const getProcessor = (moduleId) => {
  const { registerProcessor } = globalThis;
  const { WamProcessor, WamParameterInfo } = globalThis.webAudioModules.getModuleScope(moduleId);
  class TestGainProcessor extends WamProcessor {
    _generateWamParameterInfo() {
      return { gain: new WamParameterInfo('gain', { type: 'float', label: 'Gain', defaultValue: 1, minValue: 0, maxValue: 2 }) };
    }

    _process(start, end, inputs, outputs) {
      const input = inputs[0];
      const output = outputs[0];
      const g = this._parameterInterpolators.gain.values;
      for (let c = 0; c < output.length; c++) {
        const src = input[c] || input[0];
        if (!src) continue;
        for (let n = start; n < end; n++) output[c][n] = src[n] * g[n];
      }
    }
  }
  try { registerProcessor(moduleId, TestGainProcessor); } catch (_) { /* already registered */ }
  return TestGainProcessor;
};

export default class TestGain extends WebAudioModule {
  _descriptor = { ...this._descriptor, identifier: `com.2110daw.testgain${NO_GUI ? '.nogui' : ''}`, name: 'Test Gain', vendor: '2110 DAW' };

  _guiModuleUrl = NO_GUI ? undefined : new URL('./gui.js', import.meta.url).href;

  async createAudioNode(initialState) {
    await WamNode.addModules(this.audioContext, this.moduleId);
    await addFunctionModule(this.audioContext.audioWorklet, getProcessor, this.moduleId);
    const node = new WamNode(this, {});
    await node._initialize();
    if (initialState) await node.setState(initialState);
    return node;
  }

  async initialize(state) {
    this.audioNode = await this.createAudioNode(state);
    this.initialized = true;
    return this;
  }
}
