const {chromium} = require('playwright'); const fs = require('fs'); const path = require('path');
(async () => {
  const dir = process.argv[2];
  const bases = [...new Set(fs.readdirSync(dir).map(f => f.replace(/\.(vs|fs)$/, '')))].sort();
  const pairs = bases.map(b => ({b, vs: fs.existsSync(path.join(dir, b + '.vs')) ? fs.readFileSync(path.join(dir, b + '.vs'), 'utf8') : null, fs: fs.existsSync(path.join(dir, b + '.fs')) ? fs.readFileSync(path.join(dir, b + '.fs'), 'utf8') : null}));
  const br = await chromium.launch({args: ['--use-gl=swiftshader', '--enable-unsafe-swiftshader', '--ignore-gpu-blocklist']});
  const pg = await br.newPage();
  await pg.setContent('<canvas id=c></canvas>');
  const res = await pg.evaluate((pairs) => {
    const gl = document.getElementById('c').getContext('webgl2');
    if (!gl) return [{b: 'NO WEBGL2'}];
    const out = [{b: 'limits', e: 'VS vectors ' + gl.getParameter(gl.MAX_VERTEX_UNIFORM_VECTORS) + ' FS vectors ' + gl.getParameter(gl.MAX_FRAGMENT_UNIFORM_VECTORS)}];
    const comp = (type, src) => { const s = gl.createShader(type); gl.shaderSource(s, src); gl.compileShader(s); return [s, gl.getShaderParameter(s, gl.COMPILE_STATUS) ? null : gl.getShaderInfoLog(s)]; };
    // a trivial partner for single-stage passes
    for (const p of pairs) {
      let e = [];
      const [v, ve] = p.vs ? comp(gl.VERTEX_SHADER, p.vs) : [null, null];
      const [f, fe] = p.fs ? comp(gl.FRAGMENT_SHADER, p.fs) : [null, null];
      if (ve) e.push('VS: ' + ve.trim().split('\n').slice(0, 3).join(' | '));
      if (fe) e.push('FS: ' + fe.trim().split('\n').slice(0, 3).join(' | '));
      if (v && f && !ve && !fe) {
        const pr = gl.createProgram(); gl.attachShader(pr, v); gl.attachShader(pr, f); gl.linkProgram(pr);
        if (!gl.getProgramParameter(pr, gl.LINK_STATUS)) e.push('LINK: ' + gl.getProgramInfoLog(pr).trim().split('\n').slice(0, 3).join(' | '));
      }
      if (e.length) out.push({b: p.b, e: e.join(' || ')});
    }
    out.push({b: 'checked', e: String(pairs.length)});
    return out;
  }, pairs);
  for (const r of res) console.log(r.b + ': ' + (r.e || ''));
  await br.close();
})();
