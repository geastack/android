#!/usr/bin/env node
import { spawnSync } from 'node:child_process'
import { existsSync } from 'node:fs'
import path from 'node:path'
import { fileURLToPath } from 'node:url'
import { inflateSync } from 'node:zlib'

const here = path.dirname(fileURLToPath(import.meta.url))
const targetDir = path.resolve(here, '..')
const androidDir = path.resolve(targetDir, '../..')
const buildScript = path.join(targetDir, 'build-android.sh')
const packageName = process.env.GEA_ANDROID_PACKAGE_NAME || 'com.geastack.apps.weather'
const city = process.env.WEATHER_UI_CITY || 'Paris'

const args = process.argv.slice(2)
let serial = process.env.GEA_ANDROID_SERIAL || ''
let skipBuild = false
for (let i = 0; i < args.length; i++) {
  const arg = args[i]
  if (arg === '--skip-build') skipBuild = true
  else if (arg === '--serial') serial = args[++i] || ''
  else if (arg.startsWith('--serial=')) serial = arg.slice('--serial='.length)
  else if (arg === '--help') usage(0)
  else usage(2, `Unknown argument: ${arg}`)
}

function usage(code, message = '') {
  if (message) console.error(message)
  console.error('usage: node android/targets/android/test/weather-ui.mjs [--serial <adb-serial>] [--skip-build]')
  process.exit(code)
}

function androidSdkTool(name) {
  const sdk = process.env.ANDROID_HOME || process.env.ANDROID_SDK_ROOT || path.join(process.env.HOME || '', 'Library/Android/sdk')
  const candidate = path.join(sdk, 'platform-tools', name)
  return existsSync(candidate) ? candidate : name
}

const adb = process.env.GEA_ANDROID_ADB || androidSdkTool('adb')

function run(command, commandArgs, options = {}) {
  const result = spawnSync(command, commandArgs, {
    cwd: options.cwd || androidDir,
    encoding: 'utf8',
    stdio: options.capture ? ['ignore', 'pipe', 'pipe'] : 'inherit',
    env: { ...process.env, ...(serial ? { GEA_ANDROID_SERIAL: serial } : {}) }
  })
  if (result.status !== 0) {
    const detail = [result.stdout, result.stderr].filter(Boolean).join('\n')
    throw new Error(`${command} ${commandArgs.join(' ')} failed${detail ? `\n${detail}` : ''}`)
  }
  return options.capture ? result.stdout : ''
}

function runBuffer(command, commandArgs, options = {}) {
  const result = spawnSync(command, commandArgs, {
    cwd: options.cwd || androidDir,
    encoding: null,
    stdio: ['ignore', 'pipe', 'pipe'],
    env: { ...process.env, ...(serial ? { GEA_ANDROID_SERIAL: serial } : {}) }
  })
  if (result.status !== 0) {
    const detail = [result.stdout, result.stderr].filter(Boolean).map((part) => part.toString('utf8')).join('\n')
    throw new Error(`${command} ${commandArgs.join(' ')} failed${detail ? `\n${detail}` : ''}`)
  }
  return result.stdout
}

function adbArgs(extra) {
  return serial ? ['-s', serial, ...extra] : extra
}

function adbRun(extra, options = {}) {
  return run(adb, adbArgs(extra), { ...options, cwd: androidDir })
}

function adbBuffer(extra) {
  return runBuffer(adb, adbArgs(extra), { cwd: androidDir })
}

function adbBestEffort(extra) {
  try {
    adbRun(extra, { capture: true })
  } catch {}
}

function sleep(ms) {
  Atomics.wait(new Int32Array(new SharedArrayBuffer(4)), 0, 0, ms)
}

function decodeXml(value) {
  return value
    .replace(/&quot;/g, '"')
    .replace(/&apos;/g, "'")
    .replace(/&lt;/g, '<')
    .replace(/&gt;/g, '>')
    .replace(/&amp;/g, '&')
}

function parseBounds(value) {
  const match = /^\[(\-?\d+),(\-?\d+)\]\[(\-?\d+),(\-?\d+)\]$/.exec(value || '')
  if (!match) return null
  const [, x0, y0, x1, y1] = match.map(Number)
  return { x0, y0, x1, y1, width: x1 - x0, height: y1 - y0, cx: Math.floor((x0 + x1) / 2), cy: Math.floor((y0 + y1) / 2) }
}

function parseDump(xml) {
  const nodes = []
  const nodeRe = /<node\b([^>]*)\/>/g
  let nodeMatch
  while ((nodeMatch = nodeRe.exec(xml)) !== null) {
    const attrs = {}
    const attrRe = /([\w:-]+)="([^"]*)"/g
    let attrMatch
    while ((attrMatch = attrRe.exec(nodeMatch[1])) !== null) {
      attrs[attrMatch[1]] = decodeXml(attrMatch[2])
    }
    attrs.boundsRect = parseBounds(attrs.bounds)
    nodes.push(attrs)
  }
  return nodes
}

function dumpUi() {
  adbRun(['shell', 'uiautomator', 'dump', '/sdcard/gea-weather-ui.xml'], { capture: true })
  const xml = adbRun(['exec-out', 'cat', '/sdcard/gea-weather-ui.xml'], { capture: true })
  return parseDump(xml)
}

function textOf(node) {
  return node.text || ''
}

function descOf(node) {
  return node['content-desc'] || ''
}

function findText(nodes, text) {
  return nodes.find((node) => textOf(node) === text)
}

function findTextContains(nodes, text) {
  return nodes.find((node) => textOf(node).includes(text))
}

function findTopText(nodes, text, maxCy = 160) {
  return nodes
    .filter((node) => textOf(node) === text && node.boundsRect && node.boundsRect.cy <= maxCy)
    .sort((a, b) => a.boundsRect.y0 - b.boundsRect.y0 || a.boundsRect.x0 - b.boundsRect.x0)[0]
}

function findRailText(nodes, text) {
  return nodes
    .filter((node) => textOf(node) === text && node.boundsRect && node.boundsRect.width > 0 && node.boundsRect.y0 >= 50 && node.boundsRect.y1 <= 120)
    .sort((a, b) => a.boundsRect.y0 - b.boundsRect.y0 || a.boundsRect.x0 - b.boundsRect.x0)[0]
}

function findEditText(nodes) {
  return nodes.find((node) => (node.class || '').includes('EditText') && node.boundsRect && node.boundsRect.width > 0 && node.boundsRect.height > 0)
}

function tap(node, label) {
  if (!node || !node.boundsRect) throw new Error(`Cannot tap missing UI node: ${label}`)
  adbRun(['shell', 'input', 'tap', String(node.boundsRect.cx), String(node.boundsRect.cy)])
}

function waitFor(label, fn, timeoutMs = 15000, intervalMs = 500) {
  const started = Date.now()
  let last
  while (Date.now() - started < timeoutMs) {
    last = fn()
    if (last) return last
    sleep(intervalMs)
  }
  throw new Error(`Timed out waiting for ${label}`)
}

function assert(condition, message) {
  if (!condition) throw new Error(message)
}

function inputMethodShown() {
  const dump = adbRun(['shell', 'dumpsys', 'input_method'], { capture: true })
  return /mInputShown=true|mIsInputViewShown=true|isInputViewShown=true|inputShown=true|mInputViewStarted=true/.test(dump)
}

function visibleForecastIcons(nodes) {
  return nodes.filter((node) => {
    if (!((node.class || '').includes('ImageView'))) return false
    const desc = descOf(node)
    if (!/^gea-image:\d+:\d+$/.test(desc)) return false
    const bounds = node.boundsRect
    if (!bounds) return false
    return bounds.width > 0 && bounds.width <= 44 && bounds.height > 0 && bounds.height <= 44 && bounds.y0 >= 340
  })
}

function visibleHourLabels(nodes) {
  return nodes
    .filter((node) => /^\d{1,2} (AM|PM)$/.test(textOf(node)) && node.boundsRect && node.boundsRect.y0 >= 390 && node.boundsRect.y1 <= 490)
    .sort((a, b) => a.boundsRect.x0 - b.boundsRect.x0 || a.boundsRect.y0 - b.boundsRect.y0)
}

function visibleForecastLabel(nodes, text) {
  return nodes
    .filter((node) => textOf(node) === text && node.boundsRect && node.boundsRect.y0 >= 390 && node.boundsRect.y1 <= 500)
    .sort((a, b) => a.boundsRect.y0 - b.boundsRect.y0 || a.boundsRect.x0 - b.boundsRect.x0)[0]
}

function visibleForecastTemps(nodes) {
  return nodes
    .filter((node) => /^-?\d+°$/.test(textOf(node)) && node.boundsRect && node.boundsRect.width > 0 && node.boundsRect.y0 >= 430 && node.boundsRect.y1 <= 500)
    .sort((a, b) => a.boundsRect.x0 - b.boundsRect.x0 || a.boundsRect.y0 - b.boundsRect.y0)
}

function parsePng(buffer) {
  if (buffer.readUInt32BE(0) !== 0x89504e47 || buffer.readUInt32BE(4) !== 0x0d0a1a0a) {
    throw new Error('Expected PNG screenshot data')
  }
  let offset = 8
  let width = 0
  let height = 0
  let colorType = 0
  const idat = []
  while (offset + 8 <= buffer.length) {
    const length = buffer.readUInt32BE(offset)
    const type = buffer.toString('ascii', offset + 4, offset + 8)
    const dataStart = offset + 8
    const dataEnd = dataStart + length
    if (type === 'IHDR') {
      width = buffer.readUInt32BE(dataStart)
      height = buffer.readUInt32BE(dataStart + 4)
      const bitDepth = buffer[dataStart + 8]
      colorType = buffer[dataStart + 9]
      assert(bitDepth === 8 && (colorType === 2 || colorType === 6), `Unsupported PNG format depth=${bitDepth} color=${colorType}`)
    } else if (type === 'IDAT') {
      idat.push(buffer.subarray(dataStart, dataEnd))
    } else if (type === 'IEND') {
      break
    }
    offset = dataEnd + 4
  }
  const bytesPerPixel = colorType === 6 ? 4 : 3
  const stride = width * bytesPerPixel
  const raw = inflateSync(Buffer.concat(idat))
  const rgba = Buffer.alloc(width * height * 4)
  let source = 0
  const previous = Buffer.alloc(stride)
  const current = Buffer.alloc(stride)
  for (let y = 0; y < height; y++) {
    const filter = raw[source++]
    raw.copy(current, 0, source, source + stride)
    source += stride
    for (let x = 0; x < stride; x++) {
      const left = x >= bytesPerPixel ? current[x - bytesPerPixel] : 0
      const up = previous[x]
      const upLeft = x >= bytesPerPixel ? previous[x - bytesPerPixel] : 0
      let add = 0
      if (filter === 1) add = left
      else if (filter === 2) add = up
      else if (filter === 3) add = Math.floor((left + up) / 2)
      else if (filter === 4) {
        const p = left + up - upLeft
        const pa = Math.abs(p - left)
        const pb = Math.abs(p - up)
        const pc = Math.abs(p - upLeft)
        add = pa <= pb && pa <= pc ? left : pb <= pc ? up : upLeft
      } else if (filter !== 0) {
        throw new Error(`Unsupported PNG filter ${filter}`)
      }
      current[x] = (current[x] + add) & 255
    }
    for (let x = 0; x < width; x++) {
      const src = x * bytesPerPixel
      const dst = (y * width + x) * 4
      rgba[dst] = current[src]
      rgba[dst + 1] = current[src + 1]
      rgba[dst + 2] = current[src + 2]
      rgba[dst + 3] = colorType === 6 ? current[src + 3] : 255
    }
    current.copy(previous)
  }
  return { width, height, rgba }
}

function pixelAt(image, x, y) {
  const px = Math.max(0, Math.min(image.width - 1, Math.round(x)))
  const py = Math.max(0, Math.min(image.height - 1, Math.round(y)))
  const index = (py * image.width + px) * 4
  return { r: image.rgba[index], g: image.rgba[index + 1], b: image.rgba[index + 2], a: image.rgba[index + 3] }
}

function luma(pixel) {
  return pixel.r * 0.2126 + pixel.g * 0.7152 + pixel.b * 0.0722
}

function isLightInk(pixel) {
  return pixel.r >= 220 && pixel.g >= 225 && pixel.b >= 230
}

function isWarmAccentInk(pixel) {
  return pixel.r >= 200 && pixel.g >= 130 && pixel.b <= 170
}

function inkBoundsInRegion(image, region, predicate = isLightInk) {
  const x0 = Math.max(0, Math.floor(region.x0))
  const y0 = Math.max(0, Math.floor(region.y0))
  const x1 = Math.min(image.width, Math.ceil(region.x1))
  const y1 = Math.min(image.height, Math.ceil(region.y1))
  let minX = Infinity
  let minY = Infinity
  let maxX = -Infinity
  let maxY = -Infinity
  let pixels = 0
  for (let y = y0; y < y1; y++) {
    for (let x = x0; x < x1; x++) {
      const pixel = pixelAt(image, x, y)
      if (!predicate(pixel)) continue
      minX = Math.min(minX, x)
      minY = Math.min(minY, y)
      maxX = Math.max(maxX, x + 1)
      maxY = Math.max(maxY, y + 1)
      pixels++
    }
  }
  return pixels > 0 ? { x0: minX, y0: minY, x1: maxX, y1: maxY, width: maxX - minX, height: maxY - minY, pixels } : null
}

function assertCircularAddButton(nodes) {
  const plus = findText(nodes, '+')
  assert(plus && plus.boundsRect, 'Expected visible + add button text')
  const image = parsePng(adbBuffer(['exec-out', 'screencap', '-p']))
  const centerFill = pixelAt(image, plus.boundsRect.cx - 10, plus.boundsRect.cy)
  const roundedCorner = pixelAt(image, plus.boundsRect.cx - 12, plus.boundsRect.cy - 12)
  assert(isLightInk(centerFill), 'Expected add button light circular fill near center')
  assert(!isLightInk(roundedCorner), 'Expected add button corner to be rounded, not square')
}

function assertHomeTextLineBoxes(nodes) {
  const degree = nodes.find((node) => textOf(node) === '°' && node.boundsRect && node.boundsRect.y0 >= 180 && node.boundsRect.y1 <= 320)
  assert(degree, 'Expected standalone big-temperature degree mark')
  assert(degree.boundsRect.height >= 20, 'Expected degree mark to keep browser-like line-height')
  const feels = nodes.find((node) => textOf(node) === 'FEELS' && node.boundsRect)
  const feelsValue = nodes.find((node) => /°$/.test(textOf(node)) && node.boundsRect && feels && node.boundsRect.x0 >= feels.boundsRect.x1 - 2 && node.boundsRect.y0 >= 320 && node.boundsRect.y1 <= 390)
  assert(feels && feelsValue, 'Expected FEELS metric label and value')
  assert(feelsValue.boundsRect.x0 - feels.boundsRect.x1 >= 2, 'Expected metric value padding to match browser spacing')
  assert(feelsValue.boundsRect.height >= 21, 'Expected metric value to keep browser-like line-height')
}

function assertMetricsStripAlignment(nodes) {
  const labels = ['FEELS', 'WIND', 'RAIN', 'HUMID']
    .map((label) => nodes.find((node) => textOf(node) === label && node.boundsRect && node.boundsRect.y0 >= 330 && node.boundsRect.y1 <= 390))
    .filter(Boolean)

  assert(labels.length === 4, 'Expected all four metric labels in the metrics strip')
  const labelBottoms = labels.map((node) => node.boundsRect.y1)
  assert(Math.max(...labelBottoms) - Math.min(...labelBottoms) <= 2, 'Expected metric labels to share a common baseline band')

  for (const label of labels) {
    const value = nodes
      .filter((node) => node !== label && node.boundsRect && node.boundsRect.y0 >= 330 && node.boundsRect.y1 <= 390 && node.boundsRect.x0 >= label.boundsRect.x1 - 2)
      .filter((node) => !['FEELS', 'WIND', 'RAIN', 'HUMID'].includes(textOf(node)) && !/^\|$/.test(textOf(node)))
      .sort((a, b) => a.boundsRect.x0 - b.boundsRect.x0)[0]
    assert(value, `Expected metric value aligned after ${textOf(label)}`)
    assert(Math.abs(value.boundsRect.y1 - label.boundsRect.y1) <= 3, `Expected ${textOf(label)} value to baseline-align with label`)
  }
}

function assertMetricsRulesAndChipFill(nodes) {
  const labels = ['FEELS', 'WIND', 'RAIN', 'HUMID']
    .map((label) => nodes.find((node) => textOf(node) === label && node.boundsRect && node.boundsRect.y0 >= 330 && node.boundsRect.y1 <= 390))
    .filter(Boolean)
  assert(labels.length === 4, 'Expected metric labels before checking metric rules')
  const minLabelY = Math.min(...labels.map((node) => node.boundsRect.y0))
  const maxLabelY = Math.max(...labels.map((node) => node.boundsRect.y1))
  const fullRules = nodes
    .filter((node) => (node.class || '').includes('FrameLayout') && node.boundsRect)
    .filter((node) => node.boundsRect.width >= 400 && node.boundsRect.height <= 3 && node.boundsRect.y0 >= minLabelY - 14 && node.boundsRect.y1 <= maxLabelY + 8)
    .sort((a, b) => a.boundsRect.y0 - b.boundsRect.y0)
  assert(fullRules.length >= 2, 'Expected visible top and bottom rules around the metrics strip')
  const dividers = nodes
    .filter((node) => (node.class || '').includes('FrameLayout') && node.boundsRect)
    .filter((node) => node.boundsRect.width <= 3 && node.boundsRect.height >= 12 && node.boundsRect.y0 >= minLabelY - 4 && node.boundsRect.y1 <= maxLabelY + 4)
  assert(dividers.length >= 3, 'Expected visible vertical rules between metric groups')

  const image = parsePng(adbBuffer(['exec-out', 'screencap', '-p']))
  for (const rule of [fullRules[0], fullRules[fullRules.length - 1]]) {
    const line = pixelAt(image, rule.boundsRect.cx, rule.boundsRect.cy)
    const nearby = pixelAt(image, rule.boundsRect.cx, rule.boundsRect.y0 - 3)
    assert(luma(line) - luma(nearby) >= 20, 'Expected metrics horizontal rule to be visibly lighter than the background')
  }
  const divider = dividers.sort((a, b) => a.boundsRect.x0 - b.boundsRect.x0)[0]
  const dividerLine = pixelAt(image, divider.boundsRect.cx, divider.boundsRect.cy)
  const dividerNearby = pixelAt(image, divider.boundsRect.x1 + 3, divider.boundsRect.cy)
  assert(luma(dividerLine) - luma(dividerNearby) >= 20, 'Expected metric divider rule to be visibly lighter than the background')

  const railCity = findRailText(nodes, 'Lisbon') || findRailText(nodes, city) || findRailText(nodes, 'Berlin')
  assert(railCity && railCity.boundsRect, 'Expected a visible city chip for fill check')
  const chipFill = pixelAt(image, railCity.boundsRect.x0 - 5, railCity.boundsRect.cy)
  const chipOutside = pixelAt(image, railCity.boundsRect.x0 - 11, railCity.boundsRect.cy)
  assert(luma(chipFill) - luma(chipOutside) >= 18, 'Expected visible filled background behind city chip')
}

function assertHomeGlyphPlacement(nodes) {
  const image = parsePng(adbBuffer(['exec-out', 'screencap', '-p']))
  const heroName = nodes.find((node) => /^[A-Za-z][A-Za-z .'-]+$/.test(textOf(node)) && node.boundsRect && node.boundsRect.y0 >= 110 && node.boundsRect.y1 <= 190 && node.boundsRect.height >= 40)
  assert(heroName && heroName.boundsRect, 'Expected large hero city name for glyph placement check')
  const heroInk = inkBoundsInRegion(image, heroName.boundsRect, isLightInk)
  assert(heroInk, 'Expected visible hero city glyph ink')
  const heroTopDelta = heroInk.y0 - heroName.boundsRect.y0
  assert(heroTopDelta >= -4 && heroTopDelta <= 8, `Expected hero city glyphs near CSS line-box top, got delta ${heroTopDelta}`)

  const temp = nodes.find((node) => /^\d+$/.test(textOf(node)) && node.boundsRect && node.boundsRect.y0 >= 190 && node.boundsRect.y1 <= 310 && node.boundsRect.height >= 50)
  assert(temp && temp.boundsRect, 'Expected large temperature digits for glyph placement check')
  const tempInk = inkBoundsInRegion(image, { x0: temp.boundsRect.x0, y0: temp.boundsRect.y0 - 8, x1: temp.boundsRect.x1, y1: temp.boundsRect.y1 + 8 }, isLightInk)
  assert(tempInk, 'Expected visible large temperature glyph ink')
  const tempTopDelta = tempInk.y0 - temp.boundsRect.y0
  assert(tempTopDelta >= -8 && tempTopDelta <= 8, `Expected large temperature glyphs near CSS line-box top, got delta ${tempTopDelta}`)

  const degree = nodes.find((node) => textOf(node) === '°' && node.boundsRect && node.boundsRect.y0 >= 180 && node.boundsRect.y1 <= 320)
  assert(degree && degree.boundsRect, 'Expected standalone degree mark for glyph placement check')
  const degreeInk = inkBoundsInRegion(image, { x0: degree.boundsRect.x0 - 4, y0: degree.boundsRect.y0 - 4, x1: degree.boundsRect.x1 + 4, y1: degree.boundsRect.y1 + 12 }, isWarmAccentInk)
  assert(degreeInk, 'Expected visible accent degree glyph ink')
  const degreeTopDelta = degreeInk.y0 - degree.boundsRect.y0
  assert(degreeTopDelta >= -4 && degreeTopDelta <= 9, `Expected degree glyph near CSS absolute top, got delta ${degreeTopDelta}`)
  const degreeToTempTop = degreeInk.y0 - tempInk.y0
  assert(degreeToTempTop >= 2 && degreeToTempTop <= 7, `Expected degree glyph to sit near the temperature cap height, got temp-top delta ${degreeToTempTop}`)
}

function assertCityRailSingleLine(nodes) {
  const cityNames = ['Lisbon', 'San Francisco', 'Berlin', 'Tokyo', city]
    .map((name) => findRailText(nodes, name))
    .filter(Boolean)

  assert(cityNames.length >= 2, 'Expected at least two visible city rail names')
  for (const name of cityNames) {
    const label = textOf(name)
    assert(name.boundsRect.width <= 104, `Expected city rail name "${label}" to be capped by max-width, got ${name.boundsRect.width}`)
    assert(name.boundsRect.height <= 32, `Expected city rail name "${label}" to stay on one line, got height ${name.boundsRect.height}`)
  }

  const temps = nodes
    .filter((node) => /^-?\d+°$/.test(textOf(node)) && node.boundsRect && node.boundsRect.y0 >= 50 && node.boundsRect.y1 <= 120)
    .sort((a, b) => a.boundsRect.x0 - b.boundsRect.x0 || a.boundsRect.y0 - b.boundsRect.y0)

  assert(temps.length >= 2, 'Expected at least two visible city rail temperatures')
  for (const temp of temps.slice(0, 4)) {
    assert(temp.boundsRect.height <= 32, `Expected city rail temp "${textOf(temp)}" to stay on one line, got height ${temp.boundsRect.height}`)
  }
}

function assertForecastTextLineBoxes(nodes) {
  const hourLabels = visibleHourLabels(nodes).filter((node) => node.boundsRect.width > 0)
  assert(hourLabels.length >= 4, 'Expected visible next-hour labels on the home screen')
  const firstLabel = hourLabels[0]
  assert(firstLabel.boundsRect.y0 <= 415 && firstLabel.boundsRect.y1 >= 420, 'Expected next-hour labels to sit in the browser-like vertical band')

  const temps = visibleForecastTemps(nodes)
  assert(temps.length >= 4, 'Expected visible next-hour forecast temperatures')
  const firstTemp = temps[0]
  assert(firstTemp.boundsRect.height >= 11, 'Expected next-hour temperature to keep a full browser-like line box, not clip')
  assert(firstTemp.boundsRect.y0 <= 464 && firstTemp.boundsRect.y1 >= 469, 'Expected next-hour temperature to sit on the browser-like forecast baseline')
}

function escapedInputText(value) {
  return value.replace(/ /g, '%s').replace(/[&|;<>()$`\\"]/g, '\\$&')
}

if (!skipBuild) {
  run(buildScript, ['weather', 'device', ...(serial ? ['--serial', serial] : [])], { cwd: androidDir })
}

adbRun(['start-server'], { capture: true })
adbBestEffort(['shell', 'input', 'keyevent', 'KEYCODE_WAKEUP'])
adbBestEffort(['shell', 'wm', 'dismiss-keyguard'])
adbRun(['shell', 'pm', 'clear', packageName], { capture: true })
adbRun(['shell', 'am', 'start', '-S', '-n', `${packageName}/.MainActivity`], { capture: true })

let nodes = waitFor('Weather home screen', () => {
  const next = dumpUi()
  return findText(next, 'Cities') && findText(next, 'Refresh') ? next : null
})

nodes = waitFor('initial hourly forecast icons with loaded image IDs', () => {
  const next = dumpUi()
  return visibleForecastIcons(next).length >= 6 ? next : null
}, 60000)

assertHomeTextLineBoxes(nodes)
assertMetricsStripAlignment(nodes)
assertMetricsRulesAndChipFill(nodes)
assertHomeGlyphPlacement(nodes)
assertCityRailSingleLine(nodes)
assertForecastTextLineBoxes(nodes)

const hourLabelsBefore = visibleHourLabels(nodes)
assert(hourLabelsBefore.length >= 4, 'Expected visible next-hour labels on the home screen')
const hourMomentumAnchorBefore = hourLabelsBefore.find((node) => node.boundsRect.x0 >= 90) || hourLabelsBefore[3]
adbRun([
  'shell',
  'input',
  'swipe',
  '380',
  String(hourMomentumAnchorBefore.boundsRect.cy),
  '340',
  String(hourMomentumAnchorBefore.boundsRect.cy),
  '80'
])
nodes = waitFor('next-hour momentum scroll', () => {
  const next = dumpUi()
  const anchorAfter = visibleHourLabels(next).find((node) => textOf(node) === textOf(hourMomentumAnchorBefore))
  return anchorAfter && anchorAfter.boundsRect.x0 <= hourMomentumAnchorBefore.boundsRect.x0 - 45 ? next : null
}, 2500, 150)

assertCityRailSingleLine(nodes)
const railLisbonBefore = findRailText(nodes, 'Lisbon')
assert(railLisbonBefore, 'Expected city rail chips on the home screen')
adbRun([
  'shell',
  'input',
  'swipe',
  '430',
  String(railLisbonBefore.boundsRect.cy),
  '35',
  String(railLisbonBefore.boundsRect.cy),
  '550'
])
nodes = waitFor('city rail horizontal scroll', () => {
  const next = dumpUi()
  const railLisbonAfter = findRailText(next, 'Lisbon')
  if (!railLisbonAfter) return next
  if (railLisbonAfter.boundsRect.x0 < railLisbonBefore.boundsRect.x0 - 20) return next
  return null
}, 8000, 300)

tap(findText(nodes, 'Cities'), 'Cities')

nodes = waitFor('city manager EditText', () => {
  const next = dumpUi()
  return findEditText(next) ? next : null
})

const input = findEditText(nodes)
assert(input, 'Expected native android.widget.EditText for Add city')
assert(descOf(input) === 'Add city' || textOf(input) === '' || textOf(input) === 'Add city', 'Expected Add city input hint/content description')
assertCircularAddButton(nodes)
tap(input, 'Add city input')

waitFor('focused native EditText', () => {
  const next = dumpUi()
  const focusedInput = next.find((node) => (node.class || '').includes('EditText') && node.focused === 'true')
  return focusedInput ? next : null
}, 8000)
waitFor('native soft keyboard', () => inputMethodShown(), 8000)

adbRun(['shell', 'input', 'text', escapedInputText(city)])
nodes = waitFor(`typed city "${city}" in native input`, () => {
  const next = dumpUi()
  const edit = findEditText(next)
  return edit && textOf(edit) === city ? next : null
})

waitFor(`geocoded city result for ${city}`, () => {
  const next = dumpUi()
  return findText(next, city) || findTextContains(next, `${city} is already saved`) ? next : null
}, 30000)

adbRun(['shell', 'input', 'keyevent', 'ENTER'])
nodes = waitFor(`${city} added and manager closed`, () => {
  const next = dumpUi()
  if (findEditText(next)) return null
  return findText(next, city) ? next : null
}, 30000)

for (const label of ['FEELS', 'WIND', 'RAIN', 'HUMID']) {
  assert(findText(nodes, label) || findTextContains(nodes, label), `Expected browser-parity uppercase metric label ${label}`)
}
for (const label of ['Feels', 'Wind', 'Rain', 'Humid']) {
  assert(!findText(nodes, label), `Metric label should be transformed, not title-case: ${label}`)
}
assertHomeTextLineBoxes(nodes)
assertMetricsStripAlignment(nodes)
assertMetricsRulesAndChipFill(nodes)
assertCityRailSingleLine(nodes)

nodes = waitFor('hourly forecast icons with loaded image IDs', () => {
  const next = dumpUi()
  return visibleForecastIcons(next).length >= 6 ? next : null
}, 45000)
assertForecastTextLineBoxes(nodes)

tap(findText(nodes, 'Days'), 'Days forecast tab')
nodes = waitFor('daily forecast icons with loaded image IDs', () => {
  const next = dumpUi()
  return findText(next, 'NEXT DAYS') && visibleForecastIcons(next).length >= 5 ? next : null
}, 15000)

const wednesday = visibleForecastLabel(nodes, 'Wednesday')
assert(wednesday, 'Expected Wednesday in the next-days forecast')
assert(wednesday.boundsRect.height <= 24, 'Expected Wednesday to fit on one line in the next-days forecast')

const logcat = adbRun(['logcat', '-d', '-t', '1000'], { capture: true })
assert(!/\bE AndroidRuntime\b|FATAL EXCEPTION|NetworkOnMainThreadException/.test(logcat), 'Logcat contains a fatal Android runtime error')

console.log(`Weather Android UI test passed on ${serial || 'default adb device'}: rail scroll, filled city chips, metric rules/dividers, aligned metrics strip, hourly momentum, rounded add button, browser-like text line boxes, uppercase metrics, native input, IME, city add, hourly/day icons, one-line Wednesday.`)
