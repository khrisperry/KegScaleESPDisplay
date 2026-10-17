// Authoring-only import. Normal firmware builds use generate_vessel_artwork.py
// and its checked-in, lossless image data; no browser/network is needed to build.
// Usage: NODE_PATH=<playwright modules> node tools/import_web_artwork.cjs <scale repo> <output dir>
const fs = require('fs'), path = require('path');
const { chromium } = require('playwright');
(async () => {
  const scale = process.argv[2], output = process.argv[3];
  if (!scale || !output) throw Error('Provide the Scale source directory and image output directory');
  fs.mkdirSync(output, { recursive: true });
  const shell = fs.readFileSync(path.join(scale, 'components/device_web/web_shell.js'), 'utf8');
  const start = shell.indexOf('function webVesselArtwork('), end = shell.indexOf('// Sizes and tolerance', start);
  if (start < 0 || end < 0) throw Error('Web artwork function not found');
  const artwork = new Function(shell.slice(start, end) + '; return webVesselArtwork;')();
  const template = fs.readFileSync(path.join(scale, 'components/device_web/web_shell.html'), 'utf8');
  const keg = template.match(/<svg viewBox="0 0 220 290" role="img" aria-labelledby="webKegTitle">[\s\S]*?<\/svg>/)[0];
  const browser = await chromium.launch({ executablePath: process.env.PLAYWRIGHT_CHROMIUM_EXECUTABLE || undefined, args: ['--no-sandbox','--disable-gpu','--no-zygote'] });
  const page = await browser.newPage({ viewport: { width: 220, height: 290 }, deviceScaleFactor: 1 });
  const geometries = {};
  for (const kind of ['Can','Pint','Cup','Crowler','Growler','Keg']) {
    const source = kind === 'Keg' ? keg : artwork({name:kind},'source',100);
    fs.writeFileSync(path.join(output,kind+'.svg'),source);
    geometries[kind] = {width:220,height:290,top:kind==='Keg'?42:30,bottom:kind==='Keg'?259:266,foam:kind==='Keg'?4:7};
    for (const layer of ['base','liquid','foam','front']) {
      await page.setContent('<style>html,body{margin:0;background:transparent}svg{display:block;width:220px;height:290px}</style>'+source);
      await page.evaluate(({kind,layer}) => {
        const svg = document.querySelector('svg');
        svg.style.setProperty('--panel','#121a27');
        svg.querySelectorAll('title').forEach(n=>n.remove());
        const children = [...svg.children];
        const group = svg.querySelector('g[clip-path]');
        const liquid = group.querySelector('[data-web-liquid],#webKegLiquid');
        const foam = group.querySelector('[data-web-foam],#webKegFoam');
        if (kind === 'Keg') {
          // defs, two caps, barrel, clipped interior, two front reflections/ribs.
          children.forEach((node,i) => {if(i===0) return;const keep=layer==='base'?(i<=4):layer==='front'?(i>=5):node===group;if(!keep)node.remove();});
          if(layer==='base') [...group.children].forEach(n=>{if(n!==group.firstElementChild)n.remove();});
          if(layer==='liquid'||layer==='foam') {
            const target=layer==='liquid'?liquid:foam;
            [...group.children].forEach(n=>{if(n!==target)n.remove();});
            target.setAttribute('y','42');target.setAttribute('height','217');
          }
        } else {
          children.forEach((node,i)=>{if(i===0)return;const keep=layer==='base'?(i===1||i===2):layer==='front'?(i>=4||node===group):node===group;if(!keep)node.remove();});
          if(layer==='front') [...group.children].forEach(n=>{if(n===liquid||n===foam)n.remove();});
          if(layer==='liquid'||layer==='foam') {
            const target=layer==='liquid'?liquid:foam;
            [...group.children].forEach(n=>{if(n!==target)n.remove();});
            target.setAttribute('y','30');target.setAttribute('height','236');
          }
        }
      },{kind,layer});
      await page.locator('svg').screenshot({ path:path.join(output,kind+'-'+layer+'.png'),omitBackground:true });
    }
  }
  fs.writeFileSync(path.join(output,'geometry.json'),JSON.stringify(geometries,null,2)+'\n');
  await browser.close();
})().catch(e=>{console.error(e);process.exit(1)});
