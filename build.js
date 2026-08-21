let fs = require("fs")
let path = require("path")
let child_process = require("child_process")

// Vendor-aware build script, adapted from CustomHeadsetOpenVR.
// Builds the driver (MSBuild) and the GUI (npm) and stages a release folder.
//   node build.js --vendor galaxyxr   -> GalaxyXRNative vendor driver
//   node build.js                     -> vendor-neutral CustomHeadsetOpenVR

// Argument parsing
let args = process.argv.slice(2)
let vendor = "galaxyxr"
let buildDriver = true
let buildGui = true

for (let i = 0; i < args.length; i++) {
	switch (args[i]) {
		case "--vendor":
			vendor = args[++i].toLowerCase()
			break
		case "--no-driver":
			buildDriver = false
			break
		case "--no-gui":
			buildGui = false
			break
		default:
			console.log(`Unknown argument: ${args[i]}`)
			console.log('')
			console.log('Usage: node build.js [options]')
			console.log('')
			console.log('Options:')
			console.log('  --vendor <name>       Set vendor (neutral, galaxyxr). Default: galaxyxr')
			console.log('  --no-driver           Skip building the driver')
			console.log('  --no-gui              Skip building the GUI')
			console.log('')
			process.exit(1)
	}
}

if (!["neutral", "galaxyxr"].includes(vendor)) {
	console.error(`Invalid vendor "${vendor}". Must be one of: neutral, galaxyxr`)
	process.exit(1)
}

if (vendor == "neutral") {
	vendor = ""
}

// Extract version from Config.cpp
let configCppPath = path.join(__dirname, "CustomHeadsetOpenVR", "src", "Config", "Config.cpp")
let configCppData = fs.readFileSync(configCppPath, "utf8")
let versionRegex = /std::string\s*driverVersion\s*=\s*"(\d+\.\d+\.\d+(\-[a-z0-9\.]+)?)"/
let versionMatch = configCppData.match(versionRegex)
if (!versionMatch) {
	console.error("Could not extract version from Config.cpp")
	process.exit(1)
}
let version = versionMatch[1]
console.log(`Detected version: ${version}`)
console.log(`Vendor: ${vendor || "neutral"}`)

// Determine vendor preprocessor define and driver name
let vendorDefine = ""
let driverName = "CustomHeadsetOpenVR"
switch (vendor) {
	case "galaxyxr":
		vendorDefine = "VENDOR_GALAXYXR"
		driverName = "GalaxyXRNative"
		break
	case "":
	default:
		vendorDefine = ""
		break
}

// Define output directories
let vendorTag = vendor ? `-${vendor.charAt(0).toUpperCase()}${vendor.slice(1)}` : ""
let stagingFolder = `CustomHeadset-STAGING-${version}${vendorTag}-Windows`
let outputDir = path.join(__dirname, "output", stagingFolder)
let defaultDriverOutput = path.join(__dirname, "output", "CustomHeadsetOpenVR")

console.log(`Output directory: ${outputDir}`)

// Clean previous builds
function removeRecursive(dirPath) {
	if (!fs.existsSync(dirPath)) return
	try {
		fs.rmSync(dirPath, { recursive: true, force: true })
		console.log(`Removed: ${dirPath}`)
	} catch (e) {
		console.error(`Cannot remove ${dirPath}: ${e.message}`)
		console.error("A file in this directory is in use. Close any programs using it and retry.")
		process.exit(1)
	}
}

console.log("")
console.log("=== Cleaning previous builds ===")
removeRecursive(outputDir)
removeRecursive(defaultDriverOutput)

// Find Visual Studio installation
function findVswhere() {
	let programFilesX86 = process.env["ProgramFiles(x86)"] || "C:\\Program Files (x86)"
	let vswherePath = path.join(programFilesX86, "Microsoft Visual Studio\\Installer\\vswhere.exe")
	if (fs.existsSync(vswherePath)) {
		return vswherePath
	}
	console.error("Could not find vswhere.exe. Please ensure Visual Studio 2022 is installed.")
	process.exit(1)
}

function findVisualStudio() {
	let vswhere = findVswhere()
	try {
		let output = child_process.execSync(
			`"${vswhere}" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`,
			{ encoding: "utf8", windowsHide: true }
		).trim()
		return output
	} catch (e) {
		console.error("Could not find a suitable Visual Studio installation.")
		console.error("Ensure Visual Studio 2022 with C++ desktop development workload is installed.")
		process.exit(1)
	}
}

let vsPath = findVisualStudio()
console.log(`Visual Studio path: ${vsPath}`)

// Locate MSBuild
let msbuildPath = path.join(vsPath, "MSBuild\\Current\\Bin\\MSBuild.exe")
if (!fs.existsSync(msbuildPath)) {
	console.error("Could not find MSBuild.exe at:", msbuildPath)
	process.exit(1)
}

// Compute staging paths
let driverOutput = path.join(outputDir, driverName)
let guiOutput = path.join(outputDir, "CustomHeadsetGUI")

// Create staging output directories
fs.mkdirSync(driverOutput, { recursive: true })
if (buildGui) {
	fs.mkdirSync(guiOutput, { recursive: true })
}

// Driver build task
function buildDriverTask() {
	return new Promise((resolve, reject) => {
		if (!buildDriver) {
			resolve()
			return
		}

		console.log("")
		console.log("=== Building driver ===")

		let solutionPath = path.join(__dirname, "CustomHeadsetOpenVR.sln")

		let msbuildArgs = [
			solutionPath,
			"/t:Rebuild",
			"/p:Configuration=Release",
			"/p:Platform=x64",
			`/p:IntermediateOutputPath=CustomHeadsetOpenVR\\x64\\Release\\staging\\`,
			"/p:SkipPostBuild=true",
			"/m",
			"/v:minimal",
		]

		let env = { ...process.env }
		if (vendorDefine) {
			env.ExternalCompilerOptions = `/D${vendorDefine}`
		}

		console.log("Running MSBuild for driver...", msbuildPath, msbuildArgs)

		let child = child_process.spawn(msbuildPath, msbuildArgs, {
			cwd: __dirname,
			windowsHide: true,
			env,
		})

		child.stdout.on("data", (d) => { process.stdout.write(d) })
		child.stderr.on("data", (d) => { process.stderr.write(d) })

		child.on("close", (code) => {
			if (code !== 0) {
				console.error("Driver build failed.")
				reject(new Error("Driver build failed"))
				return
			}

			console.log("Driver compilation complete.")

			// Copy DriverFiles to staging (pre-build event already copied to default output)
			if (fs.existsSync(defaultDriverOutput)) {
				try {
					child_process.execSync(
						`xcopy "${defaultDriverOutput}\\*" "${driverOutput}\\" /S /Y /I`,
						{ stdio: "inherit", windowsHide: true, cwd: __dirname }
					)
					console.log("Copied driver files to staging.")
				} catch (e) {
					console.error("Failed to copy driver files.")
				}
			}

			// Rename DLL for vendor
			let stagingDll = path.join(driverOutput, "bin", "win64", "driver_CustomHeadsetOpenVR.dll")
			let renamedDll = path.join(driverOutput, "bin", "win64", `driver_${driverName}.dll`)
			if (driverName !== "CustomHeadsetOpenVR" && fs.existsSync(stagingDll)) {
				fs.renameSync(stagingDll, renamedDll)
				console.log(`Renamed DLL to driver_${driverName}.dll`)
			}

			// Update driver.vrdrivermanifest with vendor-specific name
			let manifestPath = path.join(driverOutput, "driver.vrdrivermanifest")
			if (fs.existsSync(manifestPath)) {
				let manifest = JSON.parse(fs.readFileSync(manifestPath, "utf8"))
				manifest.name = driverName
				fs.writeFileSync(manifestPath, JSON.stringify(manifest, null, 2).replaceAll("  ", "\t"))
				console.log(`Set driver name to "${driverName}" in manifest.`)
			}

			// Update default.vrsettings: SteamVR matches its sections by driver
			// name, so the renamed driver would otherwise get no defaults
			// (losing loadPriority) while shipping defaults for the neutral
			// driver's section.
			let defaultSettingsPath = path.join(driverOutput, "resources", "settings", "default.vrsettings")
			if (driverName !== "CustomHeadsetOpenVR" && fs.existsSync(defaultSettingsPath)) {
				let defaults = JSON.parse(fs.readFileSync(defaultSettingsPath, "utf8"))
				if (defaults["driver_CustomHeadsetOpenVR"]) {
					defaults[`driver_${driverName}`] = defaults["driver_CustomHeadsetOpenVR"]
					delete defaults["driver_CustomHeadsetOpenVR"]
					if (typeof defaults[`driver_${driverName}`]["Note1"] === "string") {
						defaults[`driver_${driverName}`]["Note1"] = "The settings have moved to Appdata/Roaming/GalaxyXR/CustomHeadset/settings.json"
					}
					fs.writeFileSync(defaultSettingsPath, JSON.stringify(defaults, null, 2).replaceAll("  ", "\t"))
					console.log(`Renamed default.vrsettings section to driver_${driverName}.`)
				}
			}

			console.log("Driver build complete.")
			resolve()
		})

		child.on("error", reject)
	})
}

// GUI build task
function buildGuiTask() {
	return new Promise((resolve, reject) => {
		if (!buildGui) {
			resolve()
			return
		}

		console.log("")
		console.log("=== Building GUI ===")

		let guiDir = path.join(__dirname, "CustomHeadsetGUI")
		let env = { ...process.env, VENDOR: vendor }

		let stdout = []
		let stderr = []
		let child = child_process.spawn("npm", ["run", "build"], {
			cwd: guiDir,
			env,
			windowsHide: true,
			shell: true,
		})

		child.stdout.on("data", (d) => { process.stdout.write(d); stdout.push(d) })
		child.stderr.on("data", (d) => { process.stderr.write(d); stderr.push(d) })

		child.on("close", (code) => {
			if (code !== 0) {
				console.error("GUI build failed.")
				let out = stderr.join("")
				if (out) console.error(out)
				reject(new Error("GUI build failed"))
				return
			}

			let tauriBundleDir = path.join(__dirname, "output", "CustomHeadsetGUI", "release")
			let guiExe = "custom-headset-gui.exe"

			if (fs.existsSync(path.join(tauriBundleDir, guiExe))) {
				fs.copyFileSync(
					path.join(tauriBundleDir, guiExe),
					path.join(guiOutput, guiExe)
				)
				console.log(`Copied ${guiExe} to staging.`)
			} else {
				console.error(`Could not find ${guiExe} after build.`)
			}

			console.log("GUI build complete.")
			resolve()
		})

		child.on("error", (err) => {
			console.error("GUI spawn error:", err.message)
			reject(err)
		})
	})
}

// Clean staging directory of build artifacts
function cleanStagingDir(dir) {
	let skipExtensions = new Set([".pdb", ".exp", ".lib", ".ilk", ".lastbuildstate"])

	if (!fs.existsSync(dir)) return

	let entries = fs.readdirSync(dir, { withFileTypes: true })
	for (let entry of entries) {
		let fullPath = path.join(dir, entry.name)

		if (entry.isDirectory()) {
			cleanStagingDir(fullPath)
		} else {
			let ext = path.extname(entry.name).toLowerCase()
			if (skipExtensions.has(ext)) {
				fs.unlinkSync(fullPath)
				console.log(`Removed: ${fullPath}`)
			}
		}
	}
}

// Run both builds in parallel
async function main() {
	try {
		await Promise.all([
			buildDriverTask(),
			buildGuiTask(),
		])

		// Clean staging directory of build artifacts
		console.log("")
		console.log("=== Cleaning staging artifacts ===")
		cleanStagingDir(outputDir)
		console.log("Staging cleanup complete.")

		console.log("")
		console.log("=== Build complete ===")
		console.log(`Output: ${outputDir}`)
	} catch (e) {
		process.exit(1)
	}
}

main()
