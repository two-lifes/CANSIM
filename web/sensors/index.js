/**
 * Точка сборки реестра датчиков.
 * Чтобы добавить новый датчик — создай файл в ./types/ и добавь один import ниже.
 */
import "./types/temperature.js";
import "./types/pressure.js";
import "./types/speed.js";
import "./types/digital.js";
import "./types/constant.js";

export { registerSensorType, getSensorType, listSensorTypes } from "./registry.js";
